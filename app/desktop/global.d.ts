type TauriInvoke = <T>(
  command: string,
  args?: Record<string, unknown>,
) => Promise<T>;

interface Window {
  __TAURI__?: {
    core?: {
      invoke?: TauriInvoke;
    };
  };
}
