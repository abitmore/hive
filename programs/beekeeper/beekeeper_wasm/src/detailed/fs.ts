import type { BeekeeperModule } from "../beekeeper.js";
import { BeekeeperError } from "../errors.js";
import { safeWasmCall } from "../util/wasm_error.js";

export class BeekeeperFileSystem {
  public constructor(
    private readonly fs: BeekeeperModule['FS']
  ) {}

  public sync(): Promise<void> {
    return new Promise((resolve, reject) => {
      safeWasmCall(() => this.fs.syncfs((err?: unknown) => {
        if (err) reject(err);

        resolve(undefined);
      }));
    });
  }

  private ensureCreateDir(paths: string[]) {
    // We need an absolute path in web environment in order to create proper directories
    const dir = (process.env.ROLLUP_TARGET_ENV === "web" ? "/" : "") + paths.join("/");

    let analysis = safeWasmCall(() => this.fs.analyzePath(dir));

    if (!analysis.exists) {
      if (paths.length > 1)
        this.ensureCreateDir(paths.slice(0, -1));

      safeWasmCall(() => this.fs.mkdir(dir));
    }
  }

  public init(walletDir: string): Promise<void> {
    if(process.env.ROLLUP_TARGET_ENV === "web" && !walletDir.startsWith("/"))
      throw new BeekeeperError("Storage root directory must be an absolute path in web environment");

    const walletDirPathParts = walletDir.split("/").filter(node => !!node && node !== ".");

    if (walletDirPathParts.length === 0)
      throw new BeekeeperError("Storage root directory must not be empty");

    this.ensureCreateDir(walletDirPathParts);

    if(process.env.ROLLUP_TARGET_ENV === "web")
      safeWasmCall(() => this.fs.mount(this.fs.filesystems.IDBFS, {}, walletDir));

    return new Promise((resolve, reject) => {
      safeWasmCall(() => this.fs.syncfs(true, (err?: unknown) => {
        if (err) reject(err);

        resolve(undefined);
      }));
    });
  }
}
