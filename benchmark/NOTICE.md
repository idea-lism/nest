# Benchmark Third-Party Notices

Benchmark assets in this directory include upstream code, grammars, inputs, or copied benchmark material from third-party projects.

## Included upstream material

### PackCC
- Repo: https://github.com/arithy/packcc
- Used for:
  - `benchmark/grammars/calc.peg`
  - `benchmark/grammars/json.peg`
  - `benchmark/grammars/kotlin.peg`
  - `benchmark/inputs/compare/calc.txt`
  - `benchmark/inputs/compare/json.json`
  - `benchmark/inputs/compare/kotlin.kt`
- Snapshot commit: `43001eea5899fab824abb98e450ad1f6f681b9ac`
- License copy: `benchmark/licenses/packcc.LICENSE`

### tree-sitter-json
- Repo: https://github.com/tree-sitter/tree-sitter-json
- Used for:
  - `benchmark/grammars/json.js`
- Snapshot commit: `001c28d7a29832b06b0e831ec77845553c89b56d`
- License copy: `benchmark/licenses/tree-sitter-json.LICENSE`

### tree-sitter-kotlin
- Repo: https://github.com/tree-sitter-grammars/tree-sitter-kotlin
- Used for:
  - `benchmark/grammars/kotlin.js`
- Snapshot commit: `3dea6dfa9c0129deb7c4315afbda806c85c41667`
- License copy: `benchmark/licenses/tree-sitter-kotlin.LICENSE`

## Notes
- PackCC comparison grammars/inputs kept verbatim for benchmark comparability.
- tree-sitter grammar files copied from upstream repositories for benchmark build parity.
- See each copied license file for full terms.
