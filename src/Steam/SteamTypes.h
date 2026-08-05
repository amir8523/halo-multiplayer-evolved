// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Steam/SteamTypes.h
//
// Steamworks ABI declarations, independent of the Steamworks SDK.
//
// WHY NOT JUST USE THE SDK
//
// Valve's licence requires every developer to download the SDK themselves, which
// makes it impossible to check into a repository and a hard blocker for a
// contributor who just wants to build the mod. Meanwhile the game already ships
// steam_api64.dll (Steamworks v1.57) at
//   Engine/Binaries/ThirdParty/Steamworks/Steamv157/Win64/steam_api64.dll
// and that DLL exports the complete flat C API. Verified present:
//   SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P
//   SteamAPI_ISteamNetworkingSockets_ConnectP2P
//   SteamAPI_ISteamNetworkingSockets_SendMessages
//   SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup
//   SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes
//   SteamAPI_ISteamMatchmaking_CreateLobby / JoinLobby / SetLobbyData / ...
//   SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog / SetRichPresence
//   SteamAPI_RegisterCallback / RegisterCallResult / UnregisterCallback
//   SteamInternal_FindOrCreateUserInterface
//
// So the binding is done dynamically against the DLL the game already loaded, and
// building the mod needs nothing but a compiler.
//
// THE RISK, AND HOW IT IS CONTAINED
//
// Declaring a struct layout by hand means a mistake corrupts memory rather than
// failing to compile. Three defences:
//
//   1. Every structure carries a static_assert on its size, matching the value
//      the SDK produces for v1.57 on MSVC x64. A layout mistake is a build error.
//   2. Field offsets are asserted individually wherever a struct has non trivial
//      padding, which is where mistakes actually happen.
//   3. SteamApi::Initialize logs every size at startup, so a future SDK version
//      that changes a layout is visible in a user's log rather than silent.
//
// Everything here is packed to 8 bytes, matching the SDK's own
// #pragma pack(push, 8) around its callback structures.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mpe::steam {

// ---------------------------------------------------------------------------
// Scalars and handles
// ---------------------------------------------------------------------------

using SteamId              = std::uint64_t; ///< CSteamID, opaque 64 bit.
using SteamApiCall         = std::uint64_t; ///< SteamAPICall_t.
using HSteamPipe           = std::int32_t;
using HSteamUser           = std::int32_t;
using HSteamNetConnection  = std::uint32_t;
using HSteamListenSocket   = std::uint32_t;
using HSteamNetPollGroup   = std::uint32_t;
using SteamMicroseconds    = std::int64_t; ///< SteamNetworkingMicroseconds.

inline constexpr HSteamNetConnection kInvalidConnection = 0;
inline constexpr HSteamListenSocket  kInvalidListenSocket = 0;
inline constexpr HSteamNetPollGroup  kInvalidPollGroup = 0;
inline constexpr SteamApiCall        kInvalidApiCall = 0;

/// EResult, only the values this project branches on.
enum class EResult : int {
    Ok = 1,
    Fail = 2,
    NoConnection = 3,
    InvalidParam = 8,
    AccessDenied = 15,
    LimitExceeded = 25,
};

/// ELobbyType.
enum class ELobbyType : int {
    Private = 0,
    FriendsOnly = 1,
    Public = 2,
    Invisible = 3,
};

/// EChatRoomEnterResponse, the values a join can fail with.
enum class EChatRoomEnterResponse : std::uint32_t {
    Success = 1,
    DoesntExist = 2,
    NotAllowed = 3,
    Full = 4,
    Error = 5,
    Banned = 6,
    Limited = 7,
    ClanDisabled = 8,
    CommunityBan = 9,
    MemberBlockedYou = 10,
    YouBlockedMember = 11,
};

/// EChatMemberStateChange bit flags.
enum : std::uint32_t {
    kChatMemberStateChangeEntered      = 0x0001,
    kChatMemberStateChangeLeft         = 0x0002,
    kChatMemberStateChangeDisconnected = 0x0004,
    kChatMemberStateChangeKicked       = 0x0008,
    kChatMemberStateChangeBanned       = 0x0010,
};

// ---------------------------------------------------------------------------
// Networking enums
// ---------------------------------------------------------------------------

enum class ESteamNetworkingConnectionState : int {
    None = 0,
    Connecting = 1,
    FindingRoute = 2,
    Connected = 3,
    ClosedByPeer = 4,
    ProblemDetectedLocally = 5,
};

/// ESteamNetConnectionEnd, the subset this project maps.
enum : int {
    kConnectionEndInvalid = 0,

    kConnectionEndApp_Min = 1000,
    kConnectionEndApp_Max = 1999,

    kConnectionEndAppException_Min = 2000,
    kConnectionEndAppException_Max = 2999,

    kConnectionEndLocal_OfflineMode        = 3003,
    kConnectionEndLocal_ManyRelayConnectivity = 3004,

    kConnectionEndRemote_Timeout   = 4003,
    kConnectionEndRemote_BadCrypt  = 4004,
    kConnectionEndRemote_BadCert   = 4005,

    kConnectionEndMisc_Generic                = 5001,
    kConnectionEndMisc_InternalError          = 5002,
    kConnectionEndMisc_Timeout                = 5003,
    kConnectionEndMisc_RelayConnectivity      = 5004,
    kConnectionEndMisc_SteamConnectivity      = 5005,
    kConnectionEndMisc_NoRelaySessionsToClient = 5006,
    kConnectionEndMisc_P2P_Rendezvous         = 5008,
};

/// Send flags for SendMessages.
enum : int {
    kSendUnreliable   = 0,
    kSendNoNagle      = 1,
    kSendNoDelay      = 4,
    kSendReliable     = 8,
};

/// SteamNetConnectionInfo_t::m_nFlags bits.
enum : int {
    kConnectionInfoFlagUnauthenticated = 1,
    kConnectionInfoFlagUnencrypted     = 2,
    kConnectionInfoFlagLoopbackBuffers = 4,
    kConnectionInfoFlagFast            = 8,
    kConnectionInfoFlagRelayed         = 16,
    kConnectionInfoFlagDualWifi        = 32,
};

/// ESteamNetworkingConfigValue, only the values this project sets.
enum class ESteamNetworkingConfigValue : int {
    SendRateMin      = 10,
    SendRateMax      = 11,
    TimeoutInitial   = 24,
    TimeoutConnected = 25,
};

/// ESteamNetworkingConfigDataType.
enum class ESteamNetworkingConfigDataType : int {
    Int32 = 1,
    Int64 = 2,
    Float = 3,
    String = 4,
    Ptr = 5,
};

/// ESteamNetworkingConfigScope.
enum class ESteamNetworkingConfigScope : int {
    Global = 1,
    SocketsInterface = 2,
    ListenSocket = 3,
    Connection = 4,
};

enum class ESteamNetworkingIdentityType : int {
    Invalid = 0,
    SteamId = 16,
    IpAddress = 1,
    GenericString = 2,
    GenericBytes = 3,
    UnknownType = 4,
};

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------

#pragma pack(push, 8)

/// SteamNetworkingIPAddr. 16 bytes of address plus a port, 18 total.
struct SteamNetworkingIPAddr {
    std::uint8_t  m_ipv6[16];
    std::uint16_t m_port;
};
static_assert(sizeof(SteamNetworkingIPAddr) == 18,
              "SteamNetworkingIPAddr must be 18 bytes");

/// SteamNetworkingIdentity. A tagged union; only the SteamID case is used here.
struct SteamNetworkingIdentity {
    ESteamNetworkingIdentityType m_eType;
    int                          m_cbSize;
    union {
        std::uint64_t         m_steamID64;
        char                  m_szGenericString[32];
        std::uint8_t          m_genericBytes[32];
        char                  m_szUnknownRawString[128];
        SteamNetworkingIPAddr m_ip;
        std::uint32_t         m_reserved[32];
    };

    void SetSteamId(SteamId id) noexcept {
        m_eType     = ESteamNetworkingIdentityType::SteamId;
        m_cbSize    = static_cast<int>(sizeof(std::uint64_t));
        m_steamID64 = id;
    }

    [[nodiscard]] bool IsSteamId() const noexcept {
        return m_eType == ESteamNetworkingIdentityType::SteamId;
    }
    [[nodiscard]] SteamId GetSteamId() const noexcept {
        return IsSteamId() ? m_steamID64 : 0;
    }
};
static_assert(sizeof(SteamNetworkingIdentity) == 136,
              "SteamNetworkingIdentity must be 136 bytes");
static_assert(offsetof(SteamNetworkingIdentity, m_cbSize) == 4, "m_cbSize offset");

/// SteamNetworkingConfigValue_t, used to pass options to socket creation.
struct SteamNetworkingConfigValue {
    ESteamNetworkingConfigValue     m_eValue;
    ESteamNetworkingConfigDataType  m_eDataType;
    union {
        std::int32_t m_int32;
        std::int64_t m_int64;
        float        m_float;
        const char*  m_string;
        void*        m_ptr;
    } m_val;

    void SetInt32(ESteamNetworkingConfigValue value, std::int32_t data) noexcept {
        m_eValue      = value;
        m_eDataType   = ESteamNetworkingConfigDataType::Int32;
        m_val.m_int32 = data;
    }
};
static_assert(sizeof(SteamNetworkingConfigValue) == 16,
              "SteamNetworkingConfigValue must be 16 bytes");
static_assert(offsetof(SteamNetworkingConfigValue, m_val) == 8, "m_val offset");

/// SteamNetworkingMessage_t.
///
/// Release() is a function pointer inside the struct rather than a method, which
/// is exactly why this can be bound without the SDK.
struct SteamNetworkingMessage {
    void*                   m_pData;
    int                     m_cbSize;
    HSteamNetConnection     m_conn;
    SteamNetworkingIdentity m_identityPeer;
    std::int64_t            m_nConnUserData;
    SteamMicroseconds       m_usecTimeReceived;
    std::int64_t            m_nMessageNumber;
    void (*m_pfnFreeData)(SteamNetworkingMessage*);
    void (*m_pfnRelease)(SteamNetworkingMessage*);
    int                     m_nChannel;
    int                     m_nFlags;
    std::int64_t            m_nUserData;
    std::uint16_t           m_idxLane;
    std::uint16_t           m_pad1;

    /// Returns the message to Steam. Must be called for every received message.
    void Release() {
        if (m_pfnRelease != nullptr) {
            m_pfnRelease(this);
        }
    }
};
static_assert(sizeof(SteamNetworkingMessage) == 216,
              "SteamNetworkingMessage must be 216 bytes");
static_assert(offsetof(SteamNetworkingMessage, m_conn) == 12, "m_conn offset");
static_assert(offsetof(SteamNetworkingMessage, m_identityPeer) == 16, "m_identityPeer offset");
static_assert(offsetof(SteamNetworkingMessage, m_pfnRelease) == 184, "m_pfnRelease offset");
static_assert(offsetof(SteamNetworkingMessage, m_nFlags) == 196, "m_nFlags offset");
static_assert(offsetof(SteamNetworkingMessage, m_idxLane) == 208, "m_idxLane offset");

/// SteamNetConnectionInfo_t.
struct SteamNetConnectionInfo {
    SteamNetworkingIdentity         m_identityRemote;
    std::int64_t                    m_nUserData;
    HSteamListenSocket              m_hListenSocket;
    SteamNetworkingIPAddr           m_addrRemote;
    std::uint16_t                   m_pad1;
    int                             m_idPOPRemote;
    int                             m_idPOPRelay;
    ESteamNetworkingConnectionState m_eState;
    int                             m_eEndReason;
    char                            m_szEndDebug[128];
    char                            m_szConnectionDescription[128];
    int                             m_nFlags;
    std::uint32_t                   reserved[63];
};
static_assert(sizeof(SteamNetConnectionInfo) == 696,
              "SteamNetConnectionInfo must be 696 bytes");
static_assert(offsetof(SteamNetConnectionInfo, m_nUserData) == 136, "m_nUserData offset");
static_assert(offsetof(SteamNetConnectionInfo, m_hListenSocket) == 144, "m_hListenSocket offset");
static_assert(offsetof(SteamNetConnectionInfo, m_eState) == 176, "m_eState offset");
static_assert(offsetof(SteamNetConnectionInfo, m_eEndReason) == 180, "m_eEndReason offset");
static_assert(offsetof(SteamNetConnectionInfo, m_szEndDebug) == 184, "m_szEndDebug offset");
static_assert(offsetof(SteamNetConnectionInfo, m_nFlags) == 440, "m_nFlags offset");

/// SteamNetConnectionRealTimeStatus_t.
struct SteamNetConnectionRealTimeStatus {
    ESteamNetworkingConnectionState m_eState;
    int               m_nPing;
    float             m_flConnectionQualityLocal;
    float             m_flConnectionQualityRemote;
    float             m_flOutPacketsPerSec;
    float             m_flOutBytesPerSec;
    float             m_flInPacketsPerSec;
    float             m_flInBytesPerSec;
    int               m_nSendRateBytesPerSecond;
    int               m_cbPendingUnreliable;
    int               m_cbPendingReliable;
    int               m_cbSentUnackedReliable;
    SteamMicroseconds m_usecQueueTime;
    std::uint32_t     reserved[16];
};
static_assert(sizeof(SteamNetConnectionRealTimeStatus) == 120,
              "SteamNetConnectionRealTimeStatus must be 120 bytes");
static_assert(offsetof(SteamNetConnectionRealTimeStatus, m_usecQueueTime) == 48,
              "m_usecQueueTime offset");

// ---------------------------------------------------------------------------
// Callback payloads
// ---------------------------------------------------------------------------
//
// k_iCallback values are the SDK's, which are part of the wire contract between
// steam_api64.dll and a registered callback and therefore stable.

inline constexpr int kSteamFriendsCallbacksBase             = 300;
inline constexpr int kSteamMatchmakingCallbacksBase         = 500;
inline constexpr int kSteamNetworkingSocketsCallbacksBase   = 1220;

struct SteamNetConnectionStatusChangedCallback {
    static constexpr int kCallbackId = kSteamNetworkingSocketsCallbacksBase + 1; // 1221

    HSteamNetConnection             m_hConn;
    SteamNetConnectionInfo          m_info;
    ESteamNetworkingConnectionState m_eOldState;
};
static_assert(offsetof(SteamNetConnectionStatusChangedCallback, m_info) == 8,
              "m_info offset: m_hConn is followed by 4 bytes of padding");
static_assert(sizeof(SteamNetConnectionStatusChangedCallback) == 712,
              "SteamNetConnectionStatusChangedCallback must be 712 bytes");

/// Result of RequestLobbyList.
///
/// Steam does not hand back the lobbies themselves, only how many matched. They are then
/// read by index with GetLobbyByIndex, and the set stays addressable until the next search.
struct LobbyMatchListCallback {
    static constexpr int kCallbackId = kSteamMatchmakingCallbacksBase + 10; // 510

    std::uint32_t m_nLobbiesMatching;
};
static_assert(sizeof(LobbyMatchListCallback) == 4, "LobbyMatchListCallback must be 4 bytes");

struct LobbyCreatedCallback {
    static constexpr int kCallbackId = kSteamMatchmakingCallbacksBase + 13; // 513

    EResult       m_eResult;
    std::uint64_t m_ulSteamIDLobby;
};
static_assert(sizeof(LobbyCreatedCallback) == 16, "LobbyCreatedCallback must be 16 bytes");
static_assert(offsetof(LobbyCreatedCallback, m_ulSteamIDLobby) == 8, "lobby id offset");

struct LobbyEnterCallback {
    static constexpr int kCallbackId = kSteamMatchmakingCallbacksBase + 4; // 504

    std::uint64_t m_ulSteamIDLobby;
    std::uint32_t m_rgfChatPermissions;
    bool          m_bLocked;
    std::uint32_t m_EChatRoomEnterResponse;
};
static_assert(sizeof(LobbyEnterCallback) == 24, "LobbyEnterCallback must be 24 bytes");
static_assert(offsetof(LobbyEnterCallback, m_EChatRoomEnterResponse) == 16,
              "enter response offset");

struct LobbyDataUpdateCallback {
    static constexpr int kCallbackId = kSteamMatchmakingCallbacksBase + 5; // 505

    std::uint64_t m_ulSteamIDLobby;
    std::uint64_t m_ulSteamIDMember;
    std::uint8_t  m_bSuccess;
};
static_assert(sizeof(LobbyDataUpdateCallback) == 24, "LobbyDataUpdateCallback must be 24 bytes");

struct LobbyChatUpdateCallback {
    static constexpr int kCallbackId = kSteamMatchmakingCallbacksBase + 6; // 506

    std::uint64_t m_ulSteamIDLobby;
    std::uint64_t m_ulSteamIDUserChanged;
    std::uint64_t m_ulSteamIDMakingChange;
    std::uint32_t m_rgfChatMemberStateChange;
};
static_assert(sizeof(LobbyChatUpdateCallback) == 32, "LobbyChatUpdateCallback must be 32 bytes");

struct GameLobbyJoinRequestedCallback {
    static constexpr int kCallbackId = kSteamFriendsCallbacksBase + 33; // 333

    SteamId m_steamIDLobby;
    SteamId m_steamIDFriend;
};
static_assert(sizeof(GameLobbyJoinRequestedCallback) == 16,
              "GameLobbyJoinRequestedCallback must be 16 bytes");

inline constexpr std::size_t kMaxRichPresenceValueLength = 256;

struct GameRichPresenceJoinRequestedCallback {
    static constexpr int kCallbackId = kSteamFriendsCallbacksBase + 37; // 337

    SteamId m_steamIDFriend;
    char    m_rgchConnect[kMaxRichPresenceValueLength];
};
static_assert(sizeof(GameRichPresenceJoinRequestedCallback) == 264,
              "GameRichPresenceJoinRequestedCallback must be 264 bytes");

#pragma pack(pop)

} // namespace mpe::steam
