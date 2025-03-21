import type { MainModule } from "../build/beekeeper_wasm.common";
import { BeekeeperError } from "./errors.js";
import { safeWasmCall } from "./util/wasm_error.js";

export class BeekeeperFileSystem {
  public constructor(
    private readonly fs: MainModule['FS'],
    private readonly isWebEnvironment: boolean
  ) {}

  public sync(populate = false): Promise<void> {
    return new Promise((resolve, reject) => {
      safeWasmCall(() => this.fs.syncfs(populate, (err?: unknown) => {
        if (err) reject(err);

        resolve(undefined);
      }), "filesystem sync");
    });
  }

  private ensureCreateDir(paths: string[], isAbsolutePath: boolean) {
    // We need an absolute path in web environment in order to create proper directories
    const dir = (isAbsolutePath ? "/" : "") + paths.join("/");

    let analysis = safeWasmCall(() => this.fs.analyzePath(dir, false), `analyzing path: '${dir}'`);

    if (!analysis.exists) {
      if (paths.length > 1)
        this.ensureCreateDir(paths.slice(0, -1), isAbsolutePath);

      safeWasmCall(() => this.fs.mkdir(dir), `directory creation: '${dir}'`);
    }
  }

  public async init(walletDir: string): Promise<void> {
    const isAbsolutePath = walletDir.startsWith("/");

    if(this.isWebEnvironment && !isAbsolutePath)
      throw new BeekeeperError("Storage root directory must be an absolute path in web environment");

    const walletDirPathParts = walletDir.split("/").filter(node => !!node && node !== ".");

    if (walletDirPathParts.length === 0)
      throw new BeekeeperError("Storage root directory must not be empty");

    this.ensureCreateDir(walletDirPathParts, isAbsolutePath);

    if(this.isWebEnvironment)
      safeWasmCall(() => this.fs.mount(this.fs.filesystems.IDBFS, {}, walletDir), "mounting IDBFS");

    await this.sync(true);
  }
}
