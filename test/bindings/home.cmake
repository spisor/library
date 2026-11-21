# Lays out a fresh home for one bindings test under BASE: a registered loader `test-host` whose settings enable the
# plugins PLUGINS names (comma separated, each a folder of PLUGIN_SOURCES holding its plugin.json), each installed as
# version 1.0.0 with a placeholder entry, its application settings file holding its report path, an empty local
# catalog, and the javascript-host's hint naming the copy of RUNTIME and DECLARATION it places in BASE/rt.

foreach(required BASE PLUGINS PLUGIN_SOURCES RUNTIME DECLARATION)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "home.cmake needs -D${required}")
    endif()
endforeach()

file(REMOVE_RECURSE "${BASE}")
string(REPLACE "," ";" plugin_ids "${PLUGINS}")
get_filename_component(runtime_name "${RUNTIME}" NAME)

file(COPY "${RUNTIME}" DESTINATION "${BASE}/rt")
file(COPY "${DECLARATION}" DESTINATION "${BASE}/rt")
file(MAKE_DIRECTORY "${BASE}/user" "${BASE}/portable" "${BASE}/secrets" "${BASE}/koffi")

file(WRITE "${BASE}/catalog/index.json" "{\"format\": 1, \"items\": {}}")
file(WRITE "${BASE}/state/hosts/test-host.json"
    "{\"id\": \"test-host\", \"kind\": \"host\", \"version\": \"1.0.0\", \"home\": \"${BASE}/application\"}")
file(WRITE "${BASE}/cache/hosts/javascript-host.json"
    "{\"host\": \"2.0.0\", \"libraries\": {"
    "\"runtime\": {\"version\": \"0.2.0\", \"directory\": \"${BASE}/rt\", \"binary\": \"${BASE}/rt/${runtime_name}\"}, "
    "\"koffi\": {\"version\": \"2.0.0\", \"directory\": \"${BASE}/koffi\"}}}")

set(enabled "")
foreach(id IN LISTS plugin_ids)
    set(bundle "${BASE}/cache/bundles/${id}/1.0.0")
    file(COPY "${PLUGIN_SOURCES}/${id}/plugin.json" DESTINATION "${bundle}")
    file(WRITE "${bundle}/index.js" "export default {};\n")
    file(WRITE "${bundle}/.installed.json" "{\"files\": {}}")
    file(WRITE "${BASE}/application/config/${id}.json" "{\"report\": \"${BASE}/${id}-report.txt\"}")
    list(APPEND enabled "\"${id}\": {\"enabled\": true}")
endforeach()
list(JOIN enabled ", " enabled)
file(WRITE "${BASE}/application/config/settings.json"
    "{\"plugins\": {${enabled}}, "
    "\"catalog\": {\"sources\": [{\"kind\": \"local\", \"path\": \"${BASE}/catalog/index.json\"}]}}")
