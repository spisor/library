# How an expected answer is decided

`expected.json`, `urls.json` and `folds.json` pin the catalog reader against answers decided outside
it. They are never regenerated from the reader: a file the code under test writes and a test then
compares with the same code agrees with any regression the regeneration recorded, so it pins nothing.
A new case's answer is worked out from the sources below and written by hand, and a change to the
schema changes the affected answers by the same rules.

## expected.json

Each case names a fixture, the `origin` it is read under, and whether it is `accepted`. A refused
case carries the `detail`, `path` and `fix` the refusal must give; the reader answers
`detail at path` as its message.

- **A fault the schema catches** is decided by ajv (draft-07, first error only) validating the
  fixture against `runtime/schema/catalog.schema.json`. `detail` is `the catalog index ` followed by
  ajv's message, `path` is ajv's `instancePath`, with the missing member appended for `required`.
  `fix` is chosen by the failing keyword:

  | keyword | fix |
  | --- | --- |
  | `pattern` | use the shape the schema's pattern requires: a plugin-style id, a semver version, a 64-character lowercase hex sha256, or a file key of non-empty relative segments with / separators |
  | `enum` | use one of the values the schema lists for this member |
  | `required` | add the missing member |
  | `const` | set the member to the value the schema requires |
  | `type` | give the member the type the schema requires |
  | `minimum` | use a value of at least the schema's minimum |
  | `minLength` | give the member a non-empty value |
  | `not` on an application | remove the member, which this kind of item may not carry; only a host item may have an application |
  | `anyOf` on a version | list files, platforms, or both |
  | `anyOf` on a package | give the package as a name, or as an object with a name and optional options |
  | anything else | correct the member the path names |

- **A fault the schema cannot express** (a url leaving its origin, two file keys naming one file, text
  that is not JSON) is decided by the rule `urls.json` or `folds.json` states, and
  worded as the reader's other refusals of that kind are worded.
- `detailStartsWith` replaces `detail` where the rest of the message is a parser's own description of
  the fault, which is not pinned.
- `because` says why an answer is what it is wherever the rules above would not make it obvious.

## urls.json and folds.json

Each states its oracle in `by`, and both oracles run in Node, so a new vector is decided by running
the expression `by` names on its inputs. A vector whose answer deliberately differs from that oracle
carries the answer the reader must give and its `because`.
