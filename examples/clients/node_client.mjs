// Minimal xlings interface v1 client (Node 22+).
//
// Demonstrates spawning xlings and consuming NDJSON line-by-line in JS,
// plus an inline async wrapper that turns one capability call into a
// single Promise<{events, exitCode}>.
//
// Run:
//   XLINGS_HOME=/tmp/xlings-client-test node examples/clients/node_client.mjs
//
// Mirrors examples/clients/python_client.py.

import { spawnSync } from "node:child_process";
import { existsSync, accessSync, constants } from "node:fs";
import { resolve } from "node:path";

function findXlings() {
    const candidates = [
        "build/linux/x86_64/release/xlings",
        "build/macosx/arm64/release/xlings",
        "build/macosx/x86_64/release/xlings",
    ];
    for (const c of candidates) {
        const abs = resolve(c);
        if (existsSync(abs)) {
            try { accessSync(abs, constants.X_OK); return abs; } catch {}
        }
    }
    // Fall back to PATH lookup
    const which = spawnSync("which", ["xlings"], { encoding: "utf-8" });
    const onPath = which.stdout?.trim();
    if (onPath) return onPath;
    console.error("ERROR: xlings binary not found");
    process.exit(1);
}

function callXlings(xlings, ...args) {
    const r = spawnSync(xlings, args, { encoding: "utf-8" });
    const events = [];
    for (const raw of (r.stdout ?? "").split("\n")) {
        const line = raw.trim();
        if (!line) continue;
        try { events.push(JSON.parse(line)); }
        catch { /* not NDJSON */ }
    }
    return { events, exitCode: r.status };
}

const xlings = findXlings();
console.log(`→ using ${xlings}`);

// 1. protocol_version
{
    const { events } = callXlings(xlings, "interface", "--version");
    const v = events[0]?.protocol_version;
    console.log(`protocol_version: ${v}`);
    // Major version only: a minor bump is additive.
    if (typeof v !== "string" || v.split(".")[0] !== "1") {
        console.error(`ERROR: unexpected version ${v}`);
        process.exit(1);
    }
}

// 2. capability list
{
    const { events } = callXlings(xlings, "interface", "--list");
    const caps = (events[0]?.capabilities ?? []).map(c => c.name).sort();
    console.log(`capability count: ${caps.length}`);
    for (const name of caps) console.log(`  - ${name}`);
}

// 3. env
{
    console.log("→ env:");
    const { events } = callXlings(xlings, "interface", "env", "--args", "{}");
    for (const e of events) {
        if (e.kind === "data" && e.dataKind === "env") {
            const p = e.payload;
            console.log(`  XLINGS_HOME = ${p.xlingsHome}`);
            console.log(`  activeSubos  = ${JSON.stringify(p.activeSubos)}`);
            console.log(`  binDir       = ${p.binDir}`);
        }
    }
}

// 4. list_subos
{
    console.log("→ sub-OSs:");
    const { events } = callXlings(xlings, "interface", "list_subos", "--args", "{}");
    for (const e of events) {
        if (e.kind === "data" && e.dataKind === "subos_list") {
            for (const entry of e.payload.entries ?? []) {
                const tag = entry.active ? "active" : "inactive";
                console.log(`  - ${entry.name} (${tag}, ${entry.pkgCount} pkg)`);
            }
        }
    }
}

// 5. plan_install xim:bun (dry-run)
{
    console.log("→ plan_install xim:bun (dry-run):");
    const { events } = callXlings(xlings, "interface", "plan_install",
        "--args", JSON.stringify({ targets: ["xim:bun"] }));
    let exitCode = null;
    for (const e of events) {
        if (e.kind === "data" && e.dataKind === "install_plan") {
            for (const pkg of e.payload.packages ?? []) {
                const name = Array.isArray(pkg) ? pkg[0] : pkg;
                console.log(`  + ${name}`);
            }
        } else if (e.kind === "result") {
            exitCode = e.exitCode;
        }
    }
    console.log(`plan_install exitCode: ${exitCode}`);
}

console.log("✓ node_client done");
