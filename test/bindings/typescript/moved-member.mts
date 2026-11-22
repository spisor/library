import { koffi } from "./weivel.mts";

const descriptionHead = koffi.struct({
  size: "uint32_t",
  definition: "const char *",
  types: "void *",
  type_count: "size_t",
  enums: "void *",
  enum_count: "size_t",
  operations: "void *",
  operation_count: "size_t",
  blocking: "void *",
  blocking_count: "size_t",
  definitions: "void *",
  definition_count: "size_t",
});
const describedType = koffi.struct({
  name: "const char *",
  size: "size_t",
  first_shape: "size_t",
  members: "void *",
  member_count: "size_t",
});
const describedMember = koffi.struct({
  name: "const char *",
  offset: "size_t",
  size: "size_t",
  kind: "uint32_t",
  element: "uint32_t",
  type: "void *",
  count: "void *",
  release: "void *",
});

interface TypeRow {
  readonly name: string;
  readonly members: object;
  readonly member_count: number;
}

/** Native copies a moved description is made of, freed together. */
export class CopiedDescription {
  readonly #blocks: object[] = [];

  /** `length` bytes copied from `source` into a block of its own. */
  copy(source: object, length: number): object {
    const block = koffi.alloc("uint8_t", length) as object;
    this.#blocks.push(block);
    koffi.encode(block, 0, "uint8_t", koffi.decode(source, 0, "uint8_t", length), length);
    return block;
  }

  free(): void {
    this.#blocks.splice(0).forEach((block) => koffi.free(block));
  }
}

/**
 * A copy of the `weivel_standard_description` at `description` in which the member `member` of the type `type` is
 * described `by` bytes later than the library lays it, everything else the library's own: the description, its types
 * and that type's members are copied, and the copies point at one another.
 *
 * @implNote The layout is `weivel/description.h`'s, declared here as koffi structs so the platform lays it out, as the
 * binding's own hand layout of that header is.
 */
export function withMemberMoved(copies: CopiedDescription, description: object, type: string, member: string, by: number): object {
  const head = koffi.decode(description, descriptionHead) as { size: number; types: object; type_count: number };
  const types = koffi.decode(head.types, describedType, head.type_count) as TypeRow[];
  const typeIndex = types.findIndex((row) => row.name === type);
  if (typeIndex < 0) {
    throw new Error(`the description describes no type ${type}`);
  }
  const row = types[typeIndex]!;
  const members = koffi.decode(row.members, describedMember, row.member_count) as { name: string; offset: number }[];
  const memberIndex = members.findIndex((candidate) => candidate.name === member);
  if (memberIndex < 0) {
    throw new Error(`${type} has no member ${member}`);
  }
  const memberSize = koffi.sizeof(describedMember) as number;
  const typeSize = koffi.sizeof(describedType) as number;
  const movedMembers = copies.copy(row.members, row.member_count * memberSize);
  koffi.encode(movedMembers, memberIndex * memberSize + koffi.offsetof(describedMember, "offset"), "size_t", members[memberIndex]!.offset + by);
  const movedTypes = copies.copy(head.types, head.type_count * typeSize);
  koffi.encode(movedTypes, typeIndex * typeSize + koffi.offsetof(describedType, "members"), "void *", movedMembers);
  const movedDescription = copies.copy(description, head.size);
  koffi.encode(movedDescription, koffi.offsetof(descriptionHead, "types"), "void *", movedTypes);
  return movedDescription;
}
