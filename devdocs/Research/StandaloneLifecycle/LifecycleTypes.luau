--!strict
-- Research signatures. Structural method tables describe native bindings.
-- Substitute the generated ScreenGui type for Screen in the DataModel facet.
-- A native operation returns immutable snapshots in the caller's VM.
export type ErrorCode = "Busy" | "NoSession" | "WrongState" | "NoMenu" | "InvalidMenu"
    | "NotAuthorized" | "UnsupportedTopology" | "UnsupportedAudience" | "Unavailable"
    | "InvalidOptions" | "NotFound" | "Incompatible" | "AuthenticationFailed"
    | "TimedOut" | "Disconnected" | "Cancelled" | "Retired" | "SaveInUse"
    | "StorageFull" | "UnknownOutcome" | "StartupFailed"
export type Failure = { Ok: false, Code: ErrorCode, Message: string, RecoveryId: string? }
export type Result<T> = { Ok: true, Value: T } | Failure
export type Subscription = { Disconnect: (self: Subscription) -> () }
export type Event<T> = { Connect: (self: Event<T>, Callback: (T) -> ()) -> Subscription }
export type Phase = "Validating" | "OpeningSave" | "StartingHost" | "Connecting"
    | "Authenticating" | "Loading" | "Ready" | "Retiring"
export type Progress = { Phase: Phase, Fraction: number? }
export type Operation<T> = {
    Await: (self: Operation<T>) -> Result<T>,
    GetResult: (self: Operation<T>) -> Result<T>?,
    GetProgress: (self: Operation<T>) -> Progress?,
    ProgressChanged: Event<Progress>,
    Completed: Event<Result<T>>,
    Cancel: (self: Operation<T>) -> boolean,
}
export type SessionState = "Idle" | "Starting" | "Connecting" | "Active" | "Leaving"
export type Mode = "Offline" | "PrivateHost" | "MultiplayerHost" | "Remote"
export type WorldState = "Absent" | "Preparing" | "Ready" | "Retiring"
export type SessionInfo = {
    Generation: string, State: SessionState, Mode: Mode?, World: WorldState,
    WorldId: string?, SaveId: string?, LastError: Failure?,
}
export type Save = { SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string,
    Compatibility: "Compatible" | "RequiresMigration" | "Incompatible" }
export type Page<T> = { Items: {T}, NextCursor: string? }
export type LocalOptions = { SaveId: string?, PrivateServer: boolean? }
export type HostOptions = { SaveId: string?, MaxPlayers: number?, Discovery: ("LAN" | "Node")? }
export type Listing = { Id: string, Name: string, Players: number?, Capacity: number? }
export type JoinTarget = { Kind: "Direct", Address: string } | { Kind: "Listing", Id: string }
export type SessionService = {
    GetSession: (self: SessionService) -> SessionInfo,
    Changed: Event<SessionInfo>,
    StartLocal: (self: SessionService, Options: LocalOptions?) -> Operation<SessionInfo>,
    Host: (self: SessionService, Options: HostOptions?) -> Operation<SessionInfo>,
    Join: (self: SessionService, Target: JoinTarget) -> Operation<SessionInfo>,
    FindServers: (self: SessionService, Source: "LAN" | "Node", Cursor: string?) -> Operation<Page<Listing>>,
    Leave: (self: SessionService) -> Operation<SessionInfo>,
    Reconnect: (self: SessionService) -> Operation<SessionInfo>,
}
export type WorldSaveService = {
    List: (self: WorldSaveService, Cursor: string?) -> Operation<Page<Save>>,
    Create: (self: WorldSaveService, Options: {Name: string, Seed: string?}) -> Operation<Save>,
    ResolveCreate: (self: WorldSaveService, RecoveryId: string) -> Operation<Save>,
    -- Authoritative gameplay reads the selected world's immutable metadata.
    GetCurrentWorld: (self: WorldSaveService) -> Result<Save>,
}
export type RunLifecycle = {
    PauseGameplay: (self: RunLifecycle) -> Operation<boolean>,
    ResumeGameplay: (self: RunLifecycle) -> Operation<boolean>,
    IsGameplayPaused: (self: RunLifecycle) -> boolean,
    GameplayPauseChanged: Event<boolean>,
}
export type GameLifecycle<Screen> = {
    SetMainMenu: (self: GameLifecycle<Screen>, Menu: Screen?) -> Result<boolean>,
    SetMainMenuVisible: (self: GameLifecycle<Screen>, Visible: boolean) -> Result<boolean>,
    IsMainMenuVisible: (self: GameLifecycle<Screen>) -> boolean,
    SetGameplayInputEnabled: (self: GameLifecycle<Screen>, Enabled: boolean) -> Result<boolean>,
}
export type ApplicationState = "Starting" | "Running" | "Closing" | "Closed"
export type ApplicationService = {
    GetState: (self: ApplicationService) -> ApplicationState,
    Closing: Event<boolean>, -- true once closing has been committed; advisory, no veto
    Quit: (self: ApplicationService) -> Result<boolean>,
}
export type Scalar = string | number | boolean
export type LocalPreferences = {
    Get: (self: LocalPreferences, Key: string) -> Result<Scalar?>,
    Set: (self: LocalPreferences, Key: string, Value: Scalar?) -> Operation<boolean>,
}
-- Illustrative later settings slice, populated only from native supported modes.
export type DisplayMode = { Id: string, Name: string }
export type DisplaySettings = {
    GetModes: (self: DisplaySettings) -> Result<{DisplayMode}>,
    Apply: (self: DisplaySettings, Id: string) -> Operation<boolean>,
}
return {}
