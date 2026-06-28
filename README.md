# Blueprint Importer – UE 4.26 Plugin

Recreates Blueprint graphs from a UAssetAPI JSON export **without requiring the original
`.uasset` to be in your project**. The source JSON is read, its `ScriptBytecode` is decoded,
and the equivalent K2 nodes are placed in a new Blueprint asset inside your project.

---

## Usage

1. Open any UE 4.26 project.
2. Click **File** in the top menu bar.
3. Click **Blueprint Importer…** near the bottom of the File menu.
4. In the file dialog, select a UAssetAPI JSON export (e.g. `B_Melee_Generic.json`).
5. The plugin will:
   - Parse the JSON and decode all `ScriptBytecode` arrays.
   - Create a new Blueprint asset at `/Game/ImportedBlueprints/<Name>`.
   - Populate the EventGraph and any custom function graphs with the correct K2 nodes.
   - Wire sequential exec pins.
   - Compile and save the asset.
   - Open the Blueprint editor automatically.

---

## JSON Format

The plugin reads the format produced by **UAssetAPI** (`AssetSerialize`/JSON export).
Each top-level array entry with `"Type": "Function"` that contains a `"ScriptBytecode"` array
is imported.  The `"Type": "BlueprintGeneratedClass"` entry is used to determine the asset name.

### Supported bytecode instructions

| Instruction | Node created |
|---|---|
| `EX_CallMath` | `UK2Node_CallFunction` (math/pure) |
| `EX_FinalFunction` / `EX_LocalFinalFunction` | `UK2Node_CallFunction` |
| `EX_VirtualFunction` / `EX_LocalVirtualFunction` | `UK2Node_CallFunction` |
| `EX_Context` / `EX_Context_FailSilent` | Delegates to inner `ContextExpression` node |
| `EX_DynamicCast` / `EX_ObjToInterfaceCast` | `UK2Node_DynamicCast` |
| `EX_InstanceVariable` / `EX_LocalVariable` | `UK2Node_VariableGet` |
| `EX_Let` / `EX_LetBool` / `EX_LetObj` | RHS call node or `UK2Node_VariableSet` |
| `EX_JumpIfNot` / `EX_PopExecutionFlowIfNot` | `UK2Node_IfThenElse` (Branch) |
| `EX_Self` | `UK2Node_Self` |

Flow-control-only instructions (`EX_PushExecutionFlow`, `EX_PopExecutionFlow`, `EX_Jump`,
`EX_ComputedJump`, `EX_Return`, `EX_EndOfScript`) are processed but produce no visual node.

---

## Architecture

```
BlueprintImporterModule
  ├── Registers "File > Blueprint Importer…" via UToolMenus
  ├── Opens OS file dialog (IDesktopPlatform)
  └── Orchestrates:
        FBlueprintJsonParser::Parse()   ← JSON → FBlueprintJsonData
        FBlueprintGraphBuilder::Build() ← FBlueprintJsonData → UBlueprint graphs
```

### FBlueprintJsonParser
Recursively walks the JSON object tree, filling `FBPInstruction` POD structs that mirror
the bytecode tree exactly.  No UE reflection objects are created at parse time.

### FBlueprintGraphBuilder
- **Pass 1** – Iterates instructions in statement-index order, calls `MakeNodeForInstruction()`
  which maps each instruction type to the appropriate K2 node class.
- **Pass 2** – `WireExecPins()` connects the exec chain left-to-right for all nodes that
  expose exec pins.

---

## Limitations / Known Issues

- **Data pin wiring** – Only exec pins are automatically wired.  Data/value pin connections
  are not yet wired (this requires resolving local variable lifetimes across statement
  boundaries, which is a significant additional pass).  The nodes and their pins are all
  present; you can connect data pins manually in the editor.
- **Fortnite-specific classes** – Functions referencing `FortWeapon`, `FortKismetLibrary`, etc.
  will not resolve unless those classes exist in your project/engine.  The nodes still appear
  but will show as "unresolved" (red) until the matching module is available.
- **Parent class** – The importer defaults to `AActor` as the parent class.  If the original
  Blueprint's parent is available in your project you can re-parent it via
  **Blueprint > Class Settings > Parent Class** after import.
- **UE 4.26 only** – The UToolMenus API and specific K2 node constructors used here target
  4.26.  Minor adjustments may be needed for other engine versions.

---

## Building from source

Requires:
- Unreal Engine 4.26 source or installed build with source headers
- Visual Studio 2019 (v142 toolset) or later
- Windows 10/11 (64-bit)


# Made this for Stellar, but i've parted ways so i'm releasing it. It is professionally vibecoded, credits to 00Jace for giving me the template, Andrew fixed events.
