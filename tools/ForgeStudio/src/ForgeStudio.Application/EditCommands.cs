// SPDX-License-Identifier: MIT
// Forge Studio: EditCommands.cs
//
// The concrete edits. Each stores exactly enough state to reverse itself and
// nothing more.
//
// A command captures the previous value at Apply time rather than at construction
// time. That distinction matters for redo: a command may be applied, reverted and
// applied again, and capturing at construction would make the second Apply restore
// a stale value on a later Revert.

using System;
using System.Collections.Generic;
using System.Linq;

using ForgeStudio.Domain;

namespace ForgeStudio.Application;

/// <summary>Adds one object.</summary>
public sealed class AddObjectCommand : IEditCommand
{
    private readonly ObjectPlacement _object;

    public AddObjectCommand(ObjectPlacement placement)
    {
        _object = placement ?? throw new ArgumentNullException(nameof(placement));
    }

    public string Description => $"Add {_object.PaletteKey}";

    public void Apply(MapVariant map) => map.Objects.Add(_object);

    public void Revert(MapVariant map)
    {
        // Removed by identity, not by id: an id search would find a different
        // instance if one were somehow added with the same id.
        map.Objects.Remove(_object);
    }
}

/// <summary>
/// Deletes objects by id, remembering their original positions in the list so an
/// undo restores ordering as well as content.
/// </summary>
public sealed class DeleteObjectsCommand : IEditCommand
{
    private readonly HashSet<uint> _ids;
    private readonly List<(int Index, ObjectPlacement Object)> _removed = new();

    public DeleteObjectsCommand(IEnumerable<uint> ids)
    {
        _ids = new HashSet<uint>(ids ?? throw new ArgumentNullException(nameof(ids)));
        if (_ids.Count == 0)
        {
            throw new ArgumentException("nothing to delete", nameof(ids));
        }
    }

    public string Description =>
        _ids.Count == 1 ? "Delete object" : $"Delete {_ids.Count} objects";

    public void Apply(MapVariant map)
    {
        _removed.Clear();

        // Descending, so removing an element does not shift the index of one not
        // yet visited.
        for (int i = map.Objects.Count - 1; i >= 0; i--)
        {
            if (_ids.Contains(map.Objects[i].Id))
            {
                _removed.Add((i, map.Objects[i]));
                map.Objects.RemoveAt(i);
            }
        }
    }

    public void Revert(MapVariant map)
    {
        // Ascending by original index, so each insert lands in the right place.
        foreach (var (index, placement) in _removed.OrderBy(entry => entry.Index))
        {
            var clamped = Math.Min(index, map.Objects.Count);
            map.Objects.Insert(clamped, placement);
        }
        _removed.Clear();
    }
}

/// <summary>
/// Translates objects by a delta. Coalescing: a drag gesture produces one of these
/// per mouse move, and they fold into a single undo entry.
/// </summary>
public sealed class MoveObjectsCommand : IEditCommand
{
    private readonly HashSet<uint> _ids;
    private Vector3 _delta;

    public MoveObjectsCommand(IEnumerable<uint> ids, Vector3 delta)
    {
        _ids = new HashSet<uint>(ids ?? throw new ArgumentNullException(nameof(ids)));
        _delta = delta;

        if (_ids.Count == 0)
        {
            throw new ArgumentException("nothing to move", nameof(ids));
        }
    }

    public string Description => _ids.Count == 1 ? "Move object" : $"Move {_ids.Count} objects";

    public void Apply(MapVariant map) => Translate(map, _delta);

    public void Revert(MapVariant map) => Translate(map, new Vector3(-_delta.X, -_delta.Y, -_delta.Z));

    /// <summary>
    /// Coalesces only with a move of exactly the same object set. A different
    /// selection is a different edit and deserves its own undo entry.
    /// </summary>
    public bool IsCoalescableWith(IEditCommand next) =>
        next is MoveObjectsCommand other && other._ids.SetEquals(_ids);

    public void CoalesceWith(IEditCommand next)
    {
        var other = (MoveObjectsCommand)next;
        // The other command has already been applied by the document, so only the
        // accumulated delta needs updating for a correct Revert.
        _delta = new Vector3(_delta.X + other._delta.X,
                             _delta.Y + other._delta.Y,
                             _delta.Z + other._delta.Z);
    }

    private void Translate(MapVariant map, Vector3 delta)
    {
        foreach (var placement in map.Objects.Where(o => _ids.Contains(o.Id)))
        {
            placement.Position = new Vector3(placement.Position.X + delta.X,
                                             placement.Position.Y + delta.Y,
                                             placement.Position.Z + delta.Z);
        }
        foreach (var spawn in map.Spawns.Where(s => _ids.Contains(s.Id)))
        {
            spawn.Position = new Vector3(spawn.Position.X + delta.X,
                                         spawn.Position.Y + delta.Y,
                                         spawn.Position.Z + delta.Z);
        }
        foreach (var objective in map.Objectives.Where(o => _ids.Contains(o.Id)))
        {
            objective.Position = new Vector3(objective.Position.X + delta.X,
                                             objective.Position.Y + delta.Y,
                                             objective.Position.Z + delta.Z);
        }
    }
}

/// <summary>
/// Sets one property on one object, through a getter and setter pair.
/// </summary>
/// <remarks>
/// Generic over the property type so the inspector needs one command class rather
/// than one per field. The previous value is captured at Apply time, which is what
/// makes an apply, revert, apply cycle restore correctly.
/// </remarks>
public sealed class SetPropertyCommand<T> : IEditCommand
{
    private readonly uint _objectId;
    private readonly string _propertyName;
    private readonly Func<ObjectPlacement, T> _getter;
    private readonly Action<ObjectPlacement, T> _setter;
    private readonly T _newValue;

    private T? _previousValue;
    private bool _captured;

    public SetPropertyCommand(uint objectId, string propertyName,
                              Func<ObjectPlacement, T> getter,
                              Action<ObjectPlacement, T> setter,
                              T newValue)
    {
        _objectId = objectId;
        _propertyName = propertyName ?? throw new ArgumentNullException(nameof(propertyName));
        _getter = getter ?? throw new ArgumentNullException(nameof(getter));
        _setter = setter ?? throw new ArgumentNullException(nameof(setter));
        _newValue = newValue;
    }

    public string Description => $"Set {_propertyName}";

    public void Apply(MapVariant map)
    {
        var target = Find(map);
        if (target is null)
        {
            // The object was deleted by an intervening edit. Applying is a no-op
            // rather than an exception, so a redo across a delete stays usable.
            _captured = false;
            return;
        }

        _previousValue = _getter(target);
        _captured = true;
        _setter(target, _newValue);
    }

    public void Revert(MapVariant map)
    {
        if (!_captured)
        {
            return;
        }
        var target = Find(map);
        if (target is not null)
        {
            _setter(target, _previousValue!);
        }
    }

    /// <summary>
    /// Coalesces consecutive edits to the same property of the same object, so
    /// dragging a slider is one undo entry.
    /// </summary>
    public bool IsCoalescableWith(IEditCommand next) =>
        next is SetPropertyCommand<T> other &&
        other._objectId == _objectId &&
        other._propertyName == _propertyName;

    public void CoalesceWith(IEditCommand next)
    {
        // The earlier command keeps its captured previous value, which is the one a
        // Revert must restore. The newer value is already applied to the map, so
        // there is nothing further to merge.
    }

    private ObjectPlacement? Find(MapVariant map) =>
        map.Objects.FirstOrDefault(o => o.Id == _objectId);
}

/// <summary>
/// Mirrors a set of elements across an axis, producing copies.
/// </summary>
/// <remarks>
/// Symmetry is the single most requested editor operation for competitive layouts,
/// and doing it by hand is where authors introduce the asymmetry that makes one
/// team's side subtly better.
/// </remarks>
public sealed class MirrorCommand : IEditCommand
{
    public enum Axis
    {
        X,
        Y,
    }

    private readonly List<uint> _sourceIds;
    private readonly Axis _axis;
    private readonly float _origin;
    private readonly Func<uint> _idAllocator;
    private readonly List<ObjectPlacement> _created = new();

    public MirrorCommand(IEnumerable<uint> sourceIds, Axis axis, float origin,
                         Func<uint> idAllocator)
    {
        _sourceIds = (sourceIds ?? throw new ArgumentNullException(nameof(sourceIds))).ToList();
        _axis = axis;
        _origin = origin;
        _idAllocator = idAllocator ?? throw new ArgumentNullException(nameof(idAllocator));

        if (_sourceIds.Count == 0)
        {
            throw new ArgumentException("nothing to mirror", nameof(sourceIds));
        }
    }

    public string Description => $"Mirror {_sourceIds.Count} object(s) across {_axis}";

    public void Apply(MapVariant map)
    {
        _created.Clear();

        foreach (var id in _sourceIds)
        {
            var source = map.Objects.FirstOrDefault(o => o.Id == id);
            if (source is null)
            {
                continue;
            }

            var copy = source.Clone();
            copy.Id = _idAllocator();
            copy.Position = _axis == Axis.X
                ? new Vector3(2.0f * _origin - source.Position.X, source.Position.Y, source.Position.Z)
                : new Vector3(source.Position.X, 2.0f * _origin - source.Position.Y, source.Position.Z);

            // Team assignment flips with the mirror, which is almost always what a
            // symmetric layout wants. A neutral object stays neutral.
            if (copy.Team is 0) { copy.Team = 1; }
            else if (copy.Team is 1) { copy.Team = 0; }

            map.Objects.Add(copy);
            _created.Add(copy);
        }
    }

    public void Revert(MapVariant map)
    {
        foreach (var placement in _created)
        {
            map.Objects.Remove(placement);
        }
        _created.Clear();
    }
}
