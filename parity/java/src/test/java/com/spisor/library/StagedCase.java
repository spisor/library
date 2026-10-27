package com.spisor.library;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

import java.util.List;
import java.util.Map;

/** One case laid out on disk, with the facts the library is handed for it. */
final class StagedCase {

    private final String name;
    private final String root;
    private final String home;
    private final JsonObject manifest;
    private final JsonElement names;
    private final Map<String, String> environment;
    private final JsonObject clock;
    private final List<JsonObject> calls;

    StagedCase(String name, String root, String home, JsonObject manifest, JsonElement names,
               Map<String, String> environment, JsonObject clock, List<JsonObject> calls) {
        this.name = name;
        this.root = root;
        this.home = home;
        this.manifest = manifest;
        this.names = names;
        this.environment = environment;
        this.clock = clock;
        this.calls = calls;
    }

    String name() {
        return name;
    }

    String root() {
        return root;
    }

    String home() {
        return home;
    }

    JsonObject manifest() {
        return manifest;
    }

    /** @return the case's directory names, or null when it declares none */
    JsonElement names() {
        return names;
    }

    Map<String, String> environment() {
        return environment;
    }

    JsonObject clock() {
        return clock;
    }

    List<JsonObject> calls() {
        return calls;
    }
}
