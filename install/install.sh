#!/bin/sh
set -eu

HOST_ID='spisor-host'

# Prints spisor-host's latest version, then key, url, sha256 and size per file of this platform's build,
# checked as catalog.schema.json's patterns check them, rewritten without the lookaheads awk lacks.
LIST_HOST_FILES='
function refuse(detail, fix) {
  printf "%s\nfix: %s\n", detail, fix > "/dev/stderr"
  exit 1
}
function skip_space() {
  while (at <= size && index(" \t\r\n", substr(text, at, 1)) > 0) at++
}
function peek() {
  skip_space()
  return substr(text, at, 1)
}
function hex_value(digits,    result, position, digit) {
  result = 0
  for (position = 1; position <= length(digits); position++) {
    digit = index("0123456789abcdef", tolower(substr(digits, position, 1)))
    if (digit == 0) return -1
    result = result * 16 + digit - 1
  }
  return result
}
function utf8(code) {
  if (code < 32) return "\001"
  if (code < 128) return sprintf("%c", code)
  if (code < 2048) return sprintf("%c%c", 192 + int(code / 64), 128 + code % 64)
  if (code < 65536) return sprintf("%c%c%c", 224 + int(code / 4096), 128 + int(code / 64) % 64, 128 + code % 64)
  return sprintf("%c%c%c%c", 240 + int(code / 262144), 128 + int(code / 4096) % 64, 128 + int(code / 64) % 64, 128 + code % 64)
}
function parse_string(    result, character, escaped, code, low) {
  at++
  result = ""
  while (at <= size) {
    character = substr(text, at, 1)
    if (character == "\"") {
      at++
      return result
    }
    if (character < " ") {
      broken = 1
      return ""
    }
    if (character != "\\") {
      result = result character
      at++
      continue
    }
    escaped = substr(text, at + 1, 1)
    at += 2
    if (escaped == "u") {
      code = hex_value(substr(text, at, 4))
      at += 4
      if (code >= 55296 && code < 56320 && substr(text, at, 2) == "\\u") {
        low = hex_value(substr(text, at + 2, 4))
        if (low >= 56320 && low < 57344) {
          code = 65536 + (code - 55296) * 1024 + low - 56320
          at += 6
        }
      }
      if (code < 0) {
        broken = 1
        return ""
      }
      result = result utf8(code)
    } else if (escaped == "n" || escaped == "t" || escaped == "r" || escaped == "b" || escaped == "f") {
      result = result "\001"
    } else if (escaped == "\"" || escaped == "\\" || escaped == "/") {
      result = result escaped
    } else {
      broken = 1
      return ""
    }
  }
  broken = 1
  return ""
}
function parse_literal(path,    rest) {
  rest = substr(text, at, 64)
  if (match(rest, /^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?/)) {
    kind[path] = "number"
  } else if (match(rest, /^(true|false|null)/)) {
    kind[path] = substr(rest, 1, RLENGTH)
  } else {
    return 0
  }
  value[path] = substr(rest, 1, RLENGTH)
  at += RLENGTH
  return 1
}
function parse_object(path,    count, key) {
  at++
  kind[path] = "object"
  count = 0
  if (peek() == "}") {
    at++
    members[path] = 0
    return 1
  }
  while (1) {
    if (peek() != "\"") return 0
    key = parse_string()
    if (broken || peek() != ":") return 0
    at++
    if (!((path SUBSEP key) in kind)) {
      count++
      member[path, count] = key
    }
    if (!parse_value(path SUBSEP key)) return 0
    if (peek() == ",") {
      at++
    } else if (peek() == "}") {
      at++
      members[path] = count
      return 1
    } else {
      return 0
    }
  }
}
function parse_array(path,    count) {
  at++
  kind[path] = "array"
  count = 0
  if (peek() == "]") {
    at++
    return 1
  }
  while (1) {
    count++
    if (!parse_value(path SUBSEP count)) return 0
    if (peek() == ",") {
      at++
    } else if (peek() == "]") {
      at++
      return 1
    } else {
      return 0
    }
  }
}
function parse_value(path,    character) {
  character = peek()
  if (character == "{") return parse_object(path)
  if (character == "[") return parse_array(path)
  if (character == "\"") {
    kind[path] = "string"
    value[path] = parse_string()
    return !broken
  }
  return parse_literal(path)
}
function text_at(path) {
  return kind[path] == "string" ? value[path] : ""
}
function is_version(version) {
  return version ~ /^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]*[0-9A-Za-z-])?(\+[0-9A-Za-z.-]*[0-9A-Za-z-])?$/ && index(version, "..") == 0
}
function is_file_key(key,    count, parts, position, last) {
  if (key == "" || index(key, "\\") > 0 || index(key, ":") > 0 || index(key, "\001") > 0) return 0
  count = split(key, parts, "/")
  if (substr(key, length(key), 1) == "/") return 0
  for (position = 1; position <= count; position++) {
    last = substr(parts[position], length(parts[position]), 1)
    if (parts[position] == "" || last == "." || last == " ") return 0
  }
  return 1
}
function is_sha256(text) {
  return length(text) == 64 && text !~ /[^0-9a-f]/
}
function percent_decoded(segment,    result, position, character, code) {
  result = ""
  for (position = 1; position <= length(segment); position++) {
    character = substr(segment, position, 1)
    if (character == "%") {
      code = hex_value(substr(segment, position + 1, 2))
      if (code < 0 || length(substr(segment, position + 1, 2)) < 2) return "\001"
      character = code < 32 ? "\001" : sprintf("%c", code)
      position += 2
    }
    result = result character
  }
  return result
}
function local_path(url,    count, parts, position, segment, path) {
  if (url ~ /^file:/) return file_url_path(url)
  if (url ~ /^[A-Za-z][A-Za-z0-9+.-]*:/ || substr(url, 1, 1) == "/" || url ~ /[?#\\]/) return ""
  count = split(url, parts, "/")
  path = ""
  for (position = 1; position <= count; position++) {
    segment = percent_decoded(parts[position])
    if (segment == "" || segment == "." || segment == ".." || index(segment, "/") > 0 || index(segment, "\\") > 0 || index(segment, "\001") > 0) return ""
    path = path (position > 1 ? "/" : "") segment
  }
  return path
}
function file_url_path(url,    count, parts, position, segment, path, prefix) {
  if (substr(url, 1, 8) != "file:///" || url ~ /[?#\\]/) return ""
  count = split(substr(url, 8), parts, "/")
  path = ""
  for (position = 2; position <= count; position++) {
    segment = percent_decoded(parts[position])
    if (segment == "" || segment == "." || segment == ".." || index(segment, "/") > 0 || index(segment, "\\") > 0 || index(segment, "\001") > 0) return ""
    path = path "/" segment
  }
  prefix = directory "/"
  if (substr(path, 1, length(prefix)) != prefix || length(path) == length(prefix)) return ""
  return substr(path, length(prefix) + 1)
}
function remote_url(url,    base) {
  if (url ~ /^https:\/\//) return url
  if (url ~ /^[A-Za-z][A-Za-z0-9+.-]*:/ || substr(url, 1, 2) == "//") return ""
  base = origin
  sub(/[?#].*$/, "", base)
  if (substr(url, 1, 1) == "/") {
    match(base, /^https:\/\/[^\/]*/)
    return substr(base, 1, RLENGTH) url
  }
  sub(/[^\/]*$/, "", base)
  return base url
}
function resolved(url) {
  if (url == "" || index(url, "\001") > 0) return ""
  return remote ? remote_url(url) : local_path(url)
}
function add_files(path,    position, key) {
  for (position = 1; position <= members[path]; position++) {
    key = member[path, position]
    if (key in listed) refuse("the catalog lists the " host " file \"" key "\" both for every platform and for " platform, "install from a catalog built by spisor-host catalog build")
    listed[key] = path SUBSEP key
    order[++file_count] = key
  }
}
function check_file(key, reference,    folded, url) {
  folded = tolower(key)
  if (folded in seen) refuse("the catalog lists the " host " files " seen[folded] " and " key ", which differ only by case and would install into one file", "install from a catalog built by spisor-host catalog build")
  seen[folded] = key
  url = resolved(text_at(reference SUBSEP "url"))
  if (!is_file_key(key) || folded == ".installed.json" || url == "" || !is_sha256(text_at(reference SUBSEP "sha256")) || kind[reference SUBSEP "size"] != "number" || value[reference SUBSEP "size"] !~ /^(0|[1-9][0-9]*)$/) {
    refuse("the catalog lists the " host " file \"" key "\" with a name, url, sha256 or size the installer refuses", "install from a catalog built by spisor-host catalog build")
  }
  return key "\t" url "\t" value[reference SUBSEP "sha256"] "\t" value[reference SUBSEP "size"]
}
BEGIN {
  index_file = ENVIRON["INDEX_FILE"]
  catalog = ENVIRON["CATALOG"]
  origin = catalog
  RS = "\001"
  text = ""
  if ((getline text < index_file) < 0) text = ""
  if (substr(text, 1, 3) == "\357\273\277") text = substr(text, 4)
  size = length(text)
  at = 1
  broken = 0
  parsed = parse_value("R")
  skip_space()
  if (!parsed || at <= size || kind["R"] != "object") refuse("the catalog " catalog " is not a JSON catalog index", "pass the catalog.json of a catalog, or its https url")
  item = "R" SUBSEP "items" SUBSEP host
  if (kind[item] != "object" || text_at(item SUBSEP "kind") != "host") refuse("the catalog " catalog " lists no " host, "install from a catalog that lists the host")
  version = text_at(item SUBSEP "latest")
  entry = item SUBSEP "versions" SUBSEP version
  if (!is_version(version) || kind[entry] != "object") refuse("the catalog lists no files for " host " " version, "install from a catalog whose latest " host " lists its files")
  file_count = 0
  if (kind[entry SUBSEP "files"] == "object") add_files(entry SUBSEP "files")
  build = entry SUBSEP "platforms" SUBSEP platform SUBSEP "files"
  if (kind[build] == "object") add_files(build)
  if (file_count == 0) refuse("the catalog lists no " host " " version " build for " platform, "install from a catalog that lists a " host " build for " platform)
  missing = ""
  required_count = split(required, needed, " ")
  for (position = 1; position <= required_count; position++) {
    if (!(needed[position] in listed)) missing = missing (missing == "" ? "" : ", ") needed[position]
  }
  if (missing != "") refuse("the catalog lists " host " " version " without " missing, "install from a catalog whose " host " lists " required " for " platform)
  lines = version
  for (position = 1; position <= file_count; position++) lines = lines "\n" check_file(order[position], listed[order[position]])
  print lines
}
'

refuse() {
  printf '%s\nfix: %s\n' "$1" "$2" >&2
  exit 1
}

progress() {
  printf '%s\n' "$1" >&2
}

require_tools() {
  [ "$REMOTE" != 1 ] || command -v curl >/dev/null 2>&1 || refuse 'curl is not installed, and the installer fetches with it' 'install curl, then run this again'
  if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    refuse 'neither sha256sum nor shasum is installed, and the installer verifies every download with one' 'install coreutils, then run this again'
  fi
}

file_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{ print $1 }'
  else
    shasum -a 256 "$1" | awk '{ print $1 }'
  fi
}

has_sha256() {
  [ -f "$1" ] && [ "$(file_sha256 "$1")" = "$2" ]
}

file_size() {
  wc -c < "$1" | tr -d ' '
}

is_absolute() {
  case "$1" in
    /*) return 0 ;;
    *) return 1 ;;
  esac
}

require_home() {
  is_absolute "${HOME:-}" || refuse 'HOME is absent or not an absolute path, so the cache place cannot be found' 'set HOME, or SPISOR_CACHE, to an absolute directory path'
}

resolve_platform() {
  case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=darwin ;;
    *) refuse "this installer runs on Linux and macOS, not $(uname -s)" 'on Windows, run install.ps1 in PowerShell' ;;
  esac
  case "$(uname -m)" in
    x86_64 | amd64) arch=x64 ;;
    aarch64 | arm64) arch=arm64 ;;
    *) arch=$(uname -m) ;;
  esac
  PLATFORM="$os-$arch"
}

resolve_cache() {
  if is_absolute "${SPISOR_CACHE:-}"; then
    CACHE=$SPISOR_CACHE
  elif [ "$os" = darwin ]; then
    require_home
    CACHE="$HOME/Library/Caches/spisor"
  elif is_absolute "${XDG_CACHE_HOME:-}"; then
    CACHE="$XDG_CACHE_HOME/spisor"
  else
    require_home
    CACHE="$HOME/.cache/spisor"
  fi
  CACHE=${CACHE%/}
}

check_catalog() {
  case "$CATALOG" in
    https://*) REMOTE=1 ;;
    *://*) refuse "the catalog $CATALOG is neither an https url nor a local path" 'pass the path of a catalog.json, or its https url' ;;
    *)
      REMOTE=0
      [ -f "$CATALOG" ] || refuse "the catalog $CATALOG does not exist" 'pass the path of a catalog.json, or its https url'
      case "$CATALOG" in
        */*) CATALOG_DIRECTORY=${CATALOG%/*} ;;
        *) CATALOG_DIRECTORY=. ;;
      esac
      CATALOG_ROOT=$(cd "$CATALOG_DIRECTORY" && pwd)
      ;;
  esac
}

remove_temporaries() {
  [ -z "$TEMPORARY" ] || rm -f "$TEMPORARY"
  [ -z "$INDEX" ] || [ "$REMOTE" != 1 ] || rm -f "$INDEX"
  TEMPORARY=''
}

read_catalog_index() {
  if [ "$REMOTE" = 1 ]; then
    INDEX=$(mktemp "${TMPDIR:-/tmp}/spisor-catalog.XXXXXX")
    curl -fsSL --proto '=https' --proto-redir '=https' -o "$INDEX" "$CATALOG" || refuse "could not fetch the catalog $CATALOG" 'check the network connection and the url, then run this again'
  else
    INDEX=$CATALOG
  fi
}

receive() {
  if [ "$REMOTE" = 1 ]; then
    curl -fsSL --proto '=https' --proto-redir '=https' -o "$TEMPORARY" "$1"
  else
    cp "$1" "$TEMPORARY"
  fi
}

fetch_verified() {
  TEMPORARY=$(mktemp "$2.XXXXXX")
  receive "$1" 2>/dev/null || refuse "could not fetch $1" 'check the network connection, then run this again'
  if [ "$(file_size "$TEMPORARY")" != "$4" ] || ! has_sha256 "$TEMPORARY" "$3"; then
    refuse "$1 does not have the $4 bytes and SHA-256 $3 it is pinned to" 'run this again; if it repeats, the download is being altered on the way'
  fi
  mv -f "$TEMPORARY" "$2"
  TEMPORARY=''
}

fetch_host() {
  read_catalog_index
  listing=$(INDEX_FILE=$INDEX CATALOG=$CATALOG LC_ALL=C awk -v remote="$REMOTE" -v directory="${CATALOG_ROOT:-}" -v host="$HOST_ID" -v platform="$PLATFORM" \
    -v required="$PLATFORM/$HOST_ID $PLATFORM/spisor-boot" "$LIST_HOST_FILES")
  newline='
'
  tab=$(printf '\t')
  HOST_VERSION=${listing%%"$newline"*}
  files=${listing#*"$newline"}
  VERSION_DIRECTORY="$CACHE/bundles/$HOST_ID/$HOST_VERSION"
  while IFS="$tab" read -r key url sha256 size; do
    [ "$REMOTE" = 1 ] || url="$CATALOG_DIRECTORY/$url"
    target="$VERSION_DIRECTORY/$key"
    has_sha256 "$target" "$sha256" && continue
    progress "fetching $HOST_ID $HOST_VERSION $key"
    mkdir -p "${target%/*}"
    fetch_verified "$url" "$target" "$sha256" "$size"
  done <<EOF
$files
EOF
  chmod 755 "$VERSION_DIRECTORY/$PLATFORM/$HOST_ID" "$VERSION_DIRECTORY/$PLATFORM/spisor-boot"
}

# Everything runs from main, called on the last line, so a download cut short runs nothing at all.
main() {
  TEMPORARY=''
  INDEX=''
  REMOTE=0
  CATALOG_ROOT=''
  trap remove_temporaries EXIT
  trap 'remove_temporaries; exit 130' INT
  trap 'remove_temporaries; exit 143' TERM
  [ "$#" -ge 1 ] || refuse 'no catalog was given, and none is published yet' 'pass the catalog to install from: sh install.sh <path or https url of catalog.json>'
  CATALOG=$1
  shift
  check_catalog
  require_tools
  resolve_platform
  resolve_cache
  fetch_host
  remove_temporaries
  INDEX=''
  trap - EXIT INT TERM
  exec "$VERSION_DIRECTORY/$PLATFORM/$HOST_ID" install "--catalog=$CATALOG" "$@"
}

main "$@"
