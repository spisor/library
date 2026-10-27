const loneSurrogateEscape = /\\u(d[89ab][0-9a-f]{2})(?!\\ud[c-f][0-9a-f]{2})|(?<!\\ud[89ab][0-9a-f]{2})\\u(d[c-f][0-9a-f]{2})/gi;

const controlCharacters = Array.from({ length: 31 }, (_, index) => {
  const hex = (index + 1).toString(16).padStart(4, "0");
  return `"a\\u${hex}b"`;
});

const inputs = [
  "{}",
  "[]",
  "[[]]",
  '{"a":{}}',
  '{"a":[]}',
  "[{}]",
  "[[],{},[[]]]",
  "null",
  "true",
  "false",
  "0",
  "-0",
  "0.1",
  "1e21",
  "1e-7",
  "5e-7",
  "123456789012345680000",
  "-1.5",
  "1e999",
  "[0,-0,0.5,100,1e300]",
  '""',
  '"plain"',
  '"quote \\" and backslash \\\\ and slash \\/"',
  '"\\b\\f\\n\\r\\t"',
  ...controlCharacters,
  '"\\u007f"',
  '"\u007f"',
  '"\\u2028\\u2029"',
  '"  "',
  '"é日本"',
  '"\u{1f600}"',
  '"\\ud83d\\ude00"',
  '"\\ud800"',
  '"\\udc00"',
  '"x\\ud800y"',
  '{"b":1,"10":2,"2":3,"a":4,"01":5,"4294967295":6,"4294967294":7}',
  '{"-1":1,"1.5":2,"+1":3,"0":4,"00":5,"":6}',
  '{"a":1,"b":2,"a":3}',
  '{"2":1,"1":2,"2":3}',
  '{"quo\\"te":1,"new\\nline":2,"é":3}',
  '{"a":[1,[2,{"b":[3,[]]}],{"c":{"d":[{}]}}],"e":[[[[1]]]]}',
  '[{"a":[{"b":[{"c":[null,true,false,"s",1.25]}]}]}]',
];

// spisor_parse_json reads an unpaired surrogate escape as U+FFFD, so that is the value written back.
const asParsedByTheLibrary = (text) => text.replace(loneSurrogateEscape, "\\ufffd");

const vectors = {
  generator: `node ${process.version}`,
  stringify: inputs.map((input) => ({
    input,
    expected: JSON.stringify(JSON.parse(asParsedByTheLibrary(input)), null, 2),
  })),
};

process.stdout.write(`${JSON.stringify(vectors, null, 2)}\n`);
