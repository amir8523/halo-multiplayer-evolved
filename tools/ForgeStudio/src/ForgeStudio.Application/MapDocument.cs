// SPDX-License-Identifier: MIT
// Forge Studio: MapDocument.cs
//
// The editing model. Owns the map, the undo stack and the selection.
//
// DESIGN RULE
//
// Nothing mutates the map except through an IEditCommand. The document exposes no
// setters for map state, only Execute. That single constraint is what makes undo,
// redo, atomic multi object edits, an exact dirty flag and drag coalescing all
// fall out of one mechanism instead of being retrofitted onto direct mutation,
// which never works.
//
// This type has no UI dependency. Views observe it; it never reaches back.

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;

using ForgeStudio.Domain;

namespace ForgeStudio.Application;

/// <summary>One reversible edit.</summary>
public interface IEditCommand
{
    /// <summary>Short description shown in the undo menu, for example "Move 3 objects".</summary>
    string Description { get; }

    void Apply(MapVariant map);
    void Revert(MapVariant map);

    /// <summary>
    /// True when this command can absorb <paramref name="next"/> into itself.
    /// Used so a drag produces one undo entry rather than one per mouse move.
    /// </summary>
    bool IsCoalescableWith(IEditCommand next) => false;

    /// <summary>
    /// Absorbs <paramref name="next"/>. Only called when IsCoalescableWith returned
    /// true, so an implementation may assume the cast is safe.
    /// </summary>
    void CoalesceWith(IEditCommand next) =>
        throw new NotSupportedException($"{GetType().Name} does not coalesce");
}

/// <summary>Groups several commands into one atomic undo step.</summary>
public sealed class CompositeCommand : IEditCommand
{
    private readonly List<IEditCommand> _commands;

    public CompositeCommand(string description, IEnumerable<IEditCommand> commands)
    {
        Description = description;
        _commands = commands.ToList();

        if (_commands.Count == 0)
        {
            throw new ArgumentException("a composite command needs at least one child",
                                        nameof(commands));
        }
    }

    public string Description { get; }

    public void Apply(MapVariant map)
    {
        // Forward order, and if a child throws, everything already applied is
        // reverted so the map is never left half edited.
        var applied = new List<IEditCommand>(_commands.Count);
        try
        {
            foreach (var command in _commands)
            {
                command.Apply(map);
                applied.Add(command);
            }
        }
        catch
        {
            for (int i = applied.Count - 1; i >= 0; i--)
            {
                applied[i].Revert(map);
            }
            throw;
        }
    }

    public void Revert(MapVariant map)
    {
        // Reverse order, so a child that depends on an earlier one is undone first.
        for (int i = _commands.Count - 1; i >= 0; i--)
        {
            _commands[i].Revert(map);
        }
    }
}

/// <summary>Why the document raised a change notification.</summary>
public enum DocumentChangeKind
{
    Edited,
    Undone,
    Redone,
    SelectionChanged,
    Loaded,
    Saved,
}

public sealed class DocumentChangedEventArgs : EventArgs
{
    public DocumentChangedEventArgs(DocumentChangeKind kind, string? description = null)
    {
        Kind = kind;
        Description = description;
    }

    public DocumentChangeKind Kind { get; }
    public string? Description { get; }
}

/// <summary>
/// The open map, its history and its selection.
/// </summary>
public sealed class MapDocument
{
    /// <summary>
    /// Undo depth. Bounded so a long session cannot grow without limit; 256 is
    /// far beyond what an author reaches for in practice.
    /// </summary>
    private const int MaxUndoDepth = 256;

    private readonly List<IEditCommand> _undo = new();
    private readonly List<IEditCommand> _redo = new();
    private readonly HashSet<uint> _selection = new();

    private MapDocument(MapVariant map, string? path)
    {
        Map = map;
        FilePath = path;
        _savedCommandCount = 0;
    }

    /// <summary>Command count at the last save, used for an exact dirty flag.</summary>
    private int _savedCommandCount;

    public static MapDocument CreateNew(string baseScenario)
    {
        if (string.IsNullOrWhiteSpace(baseScenario))
        {
            throw new ArgumentException("a new map needs a base scenario", nameof(baseScenario));
        }

        var map = new MapVariant
        {
            SchemaVersion = MapVariant.CurrentSchemaVersion,
            Name = "Untitled",
            BaseScenario = baseScenario,
        };
        // Slayer is the safe default: it is the only mode with no objective
        // requirements, so a brand new map validates as soon as it has two spawns.
        map.SupportedModes.Add(GameMode.Slayer);

        return new MapDocument(map, path: null);
    }

    public static MapDocument FromLoaded(MapVariant map, string path) => new(map, path);

    public MapVariant Map { get; }

    /// <summary>Null until the document has been saved once.</summary>
    public string? FilePath { get; private set; }

    /// <summary>
    /// Exact rather than a guess: true when the command count differs from the
    /// count at the last save. Undoing back to the saved state clears it.
    /// </summary>
    public bool IsDirty => _undo.Count != _savedCommandCount;

    public bool CanUndo => _undo.Count > 0;
    public bool CanRedo => _redo.Count > 0;

    public string? NextUndoDescription => CanUndo ? _undo[^1].Description : null;
    public string? NextRedoDescription => CanRedo ? _redo[^1].Description : null;

    /// <summary>Ids of the currently selected elements.</summary>
    public IReadOnlyCollection<uint> Selection => _selection;

    public event EventHandler<DocumentChangedEventArgs>? Changed;

    // ----------------------------------------------------------------------
    // Editing
    // ----------------------------------------------------------------------

    /// <summary>
    /// Applies a command and pushes it onto the undo stack.
    /// </summary>
    /// <param name="allowCoalesce">
    /// When true and the previous command accepts it, the new command is absorbed
    /// into the previous undo entry. Drag handlers pass true so a gesture is one
    /// undo step.
    /// </param>
    public void Execute(IEditCommand command, bool allowCoalesce = false)
    {
        ArgumentNullException.ThrowIfNull(command);

        command.Apply(Map);

        if (allowCoalesce && _undo.Count > 0 && _undo[^1].IsCoalescableWith(command))
        {
            _undo[^1].CoalesceWith(command);
        }
        else
        {
            _undo.Add(command);

            // A new edit invalidates the redo branch. Standard linear history: a
            // tree would be more powerful and far more confusing.
            _redo.Clear();

            if (_undo.Count > MaxUndoDepth)
            {
                _undo.RemoveAt(0);
                // The saved marker shifts with the window, otherwise trimming would
                // make a saved document look dirty.
                if (_savedCommandCount > 0)
                {
                    _savedCommandCount--;
                }
            }
        }

        Raise(DocumentChangeKind.Edited, command.Description);
    }

    public void Undo()
    {
        if (!CanUndo)
        {
            return;
        }

        var command = _undo[^1];
        _undo.RemoveAt(_undo.Count - 1);
        command.Revert(Map);
        _redo.Add(command);

        PruneSelection();
        Raise(DocumentChangeKind.Undone, command.Description);
    }

    public void Redo()
    {
        if (!CanRedo)
        {
            return;
        }

        var command = _redo[^1];
        _redo.RemoveAt(_redo.Count - 1);
        command.Apply(Map);
        _undo.Add(command);

        Raise(DocumentChangeKind.Redone, command.Description);
    }

    /// <summary>Records that the document was written to <paramref name="path"/>.</summary>
    public void MarkSaved(string path)
    {
        FilePath = path;
        _savedCommandCount = _undo.Count;
        Raise(DocumentChangeKind.Saved);
    }

    // ----------------------------------------------------------------------
    // Selection
    // ----------------------------------------------------------------------

    public void SetSelection(IEnumerable<uint> ids)
    {
        ArgumentNullException.ThrowIfNull(ids);

        var incoming = new HashSet<uint>(ids);
        if (incoming.SetEquals(_selection))
        {
            return; // No notification for a no-op, so views do not redraw needlessly.
        }

        _selection.Clear();
        foreach (var id in incoming)
        {
            _selection.Add(id);
        }
        Raise(DocumentChangeKind.SelectionChanged);
    }

    public void ToggleSelection(uint id)
    {
        if (!_selection.Remove(id))
        {
            _selection.Add(id);
        }
        Raise(DocumentChangeKind.SelectionChanged);
    }

    public void ClearSelection() => SetSelection(Array.Empty<uint>());

    /// <summary>Selected objects, skipping ids that no longer exist.</summary>
    public IEnumerable<ObjectPlacement> SelectedObjects =>
        Map.Objects.Where(o => _selection.Contains(o.Id));

    /// <summary>
    /// Drops selection entries whose element is gone, which happens after an undo
    /// of an add or a redo of a delete. Without this, a later operation on the
    /// selection would silently target nothing.
    /// </summary>
    private void PruneSelection()
    {
        if (_selection.Count == 0)
        {
            return;
        }

        var live = new HashSet<uint>(Map.Objects.Select(o => o.Id));
        live.UnionWith(Map.Spawns.Select(s => s.Id));
        live.UnionWith(Map.Objectives.Select(o => o.Id));
        live.UnionWith(Map.Boundaries.Select(b => b.Id));

        _selection.RemoveWhere(id => !live.Contains(id));
    }

    // ----------------------------------------------------------------------
    // Ids
    // ----------------------------------------------------------------------

    /// <summary>
    /// Allocates an id unused by any collection.
    /// </summary>
    /// <remarks>
    /// Ids are unique across the whole document, not per collection, so the
    /// selection set can hold a mix of element kinds without ambiguity. Allocation
    /// is max plus one rather than lowest free, so an id is never reused within a
    /// session and an undone add cannot collide with a later one.
    /// </remarks>
    public uint AllocateId()
    {
        uint highest = 0;
        foreach (var id in Map.Objects.Select(o => o.Id)
                              .Concat(Map.Spawns.Select(s => s.Id))
                              .Concat(Map.Objectives.Select(o => o.Id))
                              .Concat(Map.Boundaries.Select(b => b.Id)))
        {
            if (id > highest)
            {
                highest = id;
            }
        }

        if (highest == uint.MaxValue)
        {
            throw new InvalidOperationException("the document has exhausted its id space");
        }
        return highest + 1;
    }

    private void Raise(DocumentChangeKind kind, string? description = null) =>
        Changed?.Invoke(this, new DocumentChangedEventArgs(kind, description));
}
