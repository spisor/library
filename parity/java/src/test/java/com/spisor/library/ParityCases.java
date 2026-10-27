package com.spisor.library;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonPrimitive;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

/**
 * Reads and lays out the parity cases exactly as {@code parity/stage.mjs} does for the oracle and
 * the TypeScript driver.
 */
final class ParityCases {

    private static final String DEFAULT_START = "2026-10-27T19:34:53.123Z";

    private final Path parityDirectory;

    ParityCases(Path parityDirectory) {
        this.parityDirectory = parityDirectory;
    }

    static ParityCases fromSystemProperty() {
        return new ParityCases(Paths.get(System.getProperty("parity.directory")).toAbsolutePath().normalize());
    }

    /** {@code PARITY_CASES} points a driver at another case set, which is how the suite is shown to refuse an empty one. */
    private Path casesDirectory() {
        String override = System.getenv("PARITY_CASES");
        return override == null || override.isEmpty() ? parityDirectory.resolve("cases") : Paths.get(override).toAbsolutePath().normalize();
    }

    Path declarationFile() {
        return parityDirectory.resolve("..").resolve("runtime").resolve("library.json").normalize();
    }

    Path workDirectory(String driver) {
        return parityDirectory.resolve("work").resolve(driver);
    }

    List<String> names() {
        List<String> names = new ArrayList<String>();
        File[] entries = casesDirectory().toFile().listFiles();
        if (entries != null) {
            for (File entry : entries) {
                if (entry.isDirectory()) {
                    names.add(entry.getName());
                }
            }
        }
        Collections.sort(names);
        return names;
    }

    /** How many calls the case files declare, counted apart from any run so a run that skips one is caught. */
    int declaredCallCount(List<String> names) throws IOException {
        int calls = 0;
        for (String name : names) {
            calls += definitionOf(name).getAsJsonArray("calls").size();
        }
        return calls;
    }

    JsonObject expected(String name) throws IOException {
        return parse(parityDirectory.resolve("expected").resolve(name + ".json")).getAsJsonObject();
    }

    JsonArray divergences() throws IOException {
        JsonArray divergences = parse(parityDirectory.resolve("divergences.json")).getAsJsonObject().getAsJsonArray("divergences");
        Set<String> repeated = repeatedCalls(divergences);
        if (!repeated.isEmpty()) {
            throw new IllegalStateException("divergences.json records a call more than once: " + repeated);
        }
        return divergences;
    }

    /** The {@code case/call} pairs named more than once, since a driver could hold only one of them. */
    private static Set<String> repeatedCalls(JsonArray divergences) {
        Set<String> seen = new LinkedHashSet<String>();
        Set<String> repeated = new LinkedHashSet<String>();
        for (JsonElement element : divergences) {
            JsonObject entry = element.getAsJsonObject();
            if (!entry.has("call")) {
                continue;
            }
            String pair = entry.get("case").getAsString() + "/" + entry.get("call").getAsInt();
            if (!seen.add(pair)) {
                repeated.add(pair);
            }
        }
        return repeated;
    }

    private JsonObject definitionOf(String name) throws IOException {
        return parse(casesDirectory().resolve(name).resolve("case.json")).getAsJsonObject();
    }

    /**
     * Lays one case out afresh: its fixture files copied byte for byte into the home.
     */
    StagedCase stage(String name, Path workRoot) throws IOException {
        JsonObject definition = definitionOf(name);
        Path root = workRoot.resolve(name).toAbsolutePath().normalize();
        Path home = root.resolve("home");
        deleteTree(root);
        Files.createDirectories(home);
        copyFixtures(casesDirectory().resolve(name).resolve("home"), definition, home);
        return new StagedCase(name, root.toString(), home.toString(), definition.getAsJsonObject("manifest"),
                definition.get("names"), environmentOf(definition), clockOf(definition), callsOf(definition));
    }

    private static void copyFixtures(Path fixtures, JsonObject definition, Path home) throws IOException {
        if (!definition.has("home")) {
            return;
        }
        for (JsonElement file : definition.getAsJsonArray("home")) {
            Path target = home.resolve(file.getAsString());
            Files.createDirectories(target.getParent());
            Files.copy(fixtures.resolve(file.getAsString()), target);
        }
    }

    private static Map<String, String> environmentOf(JsonObject definition) {
        Map<String, String> environment = new LinkedHashMap<String, String>();
        if (definition.has("environment")) {
            for (Map.Entry<String, JsonElement> variable : definition.getAsJsonObject("environment").entrySet()) {
                environment.put(variable.getKey(), variable.getValue().getAsString());
            }
        }
        return environment;
    }

    private static JsonObject clockOf(JsonObject definition) {
        if (definition.has("clock")) {
            return definition.getAsJsonObject("clock");
        }
        JsonObject clock = new JsonObject();
        clock.addProperty("start", DEFAULT_START);
        clock.add("times", new JsonArray());
        return clock;
    }

    private static List<JsonObject> callsOf(JsonObject definition) {
        List<JsonObject> calls = new ArrayList<JsonObject>();
        for (JsonElement call : definition.getAsJsonArray("calls")) {
            calls.add(call.getAsJsonObject());
        }
        return calls;
    }

    /** The harness's own step between library calls, an edit a person makes by hand. */
    static boolean isHandEdit(JsonObject call) {
        return "file.write".equals(call.get("function").getAsString());
    }

    static JsonObject performHandEdit(StagedCase staged, JsonObject call) throws IOException {
        JsonObject argument = call.getAsJsonObject("argument");
        Path target = Paths.get(staged.home()).resolve(argument.get("path").getAsString());
        Files.createDirectories(target.getParent());
        write(target, argument.get("text").getAsString());
        return new JsonObject();
    }

    static String logTime(StagedCase staged, int logIndex) {
        JsonArray times = staged.clock().getAsJsonArray("times");
        return logIndex < times.size() ? times.get(logIndex).getAsString() : staged.clock().get("start").getAsString();
    }

    /** The argument the library is called with, built as {@code libraryArgument} in {@code stage.mjs} builds it. */
    static JsonObject libraryArgument(StagedCase staged, JsonObject call, int logIndex) {
        String function = call.get("function").getAsString();
        JsonObject argument = new JsonObject();
        argument.addProperty("home", staged.home());
        if (staged.names() != null) {
            argument.add("names", staged.names());
        }
        if (!"paths.resolve".equals(function)) {
            argument.add("plugin", staged.manifest().get("id"));
            if (staged.manifest().has("config")) {
                argument.add("config", staged.manifest().get("config"));
            }
        }
        if ("log.write".equals(function)) {
            argument.addProperty("start", staged.clock().get("start").getAsString());
            argument.addProperty("time", logTime(staged, logIndex));
            argument.addProperty("noColor", staged.environment().containsKey("NO_COLOR"));
        }
        for (Map.Entry<String, JsonElement> member : call.getAsJsonObject("argument").entrySet()) {
            argument.add(member.getKey(), member.getValue());
        }
        return argument;
    }

    /** An answer with the case's own directories written back as their tokens and every backslash as a slash. */
    static JsonElement normalised(JsonElement value, StagedCase staged) {
        if (value.isJsonPrimitive() && value.getAsJsonPrimitive().isString()) {
            String text = value.getAsString().replace(staged.home(), "{home}").replace(staged.root(), "{root}");
            return new JsonPrimitive(text.replace('\\', '/'));
        }
        if (value.isJsonArray()) {
            JsonArray array = new JsonArray();
            for (JsonElement element : value.getAsJsonArray()) {
                array.add(normalised(element, staged));
            }
            return array;
        }
        if (value.isJsonObject()) {
            JsonObject object = new JsonObject();
            for (Map.Entry<String, JsonElement> member : value.getAsJsonObject().entrySet()) {
                object.add(member.getKey(), normalised(member.getValue(), staged));
            }
            return object;
        }
        return value;
    }

    /** A refusal as every implementation records it: the library's message without the bridge's label, and its fix. */
    static JsonElement failureAnswer(String function, String detail, String fix, StagedCase staged) {
        String label = function + ": ";
        JsonObject failure = new JsonObject();
        failure.addProperty("message", detail.startsWith(label) ? detail.substring(label.length()) : detail);
        failure.addProperty("fix", fix);
        JsonObject answer = new JsonObject();
        answer.add("failure", failure);
        return normalised(answer, staged);
    }

    /** Every file under the home's log and config directories, as {@code homeFiles} in {@code stage.mjs} lists them. */
    static Map<String, String> homeFiles(StagedCase staged) throws IOException {
        Path home = Paths.get(staged.home());
        Map<String, String> files = new TreeMap<String, String>();
        for (String directory : comparedDirectories(staged)) {
            Path path = resolvedOrNull(home, directory);
            if (path != null && Files.isDirectory(path)) {
                filesUnder(path, home, files);
            }
        }
        return files;
    }

    private static Set<String> comparedDirectories(StagedCase staged) {
        Set<String> directories = new LinkedHashSet<String>();
        directories.add("logs");
        directories.add("config");
        JsonElement names = staged.names();
        JsonElement renamed = names != null && names.isJsonObject() ? names.getAsJsonObject().get("config") : null;
        if (renamed != null && renamed.isJsonPrimitive() && renamed.getAsJsonPrimitive().isString()) {
            directories.add(renamed.getAsString());
        }
        return directories;
    }

    /** @implNote A name Windows cannot hold in a path, such as {@code a:b}, names no directory there at all. */
    private static Path resolvedOrNull(Path home, String directory) {
        try {
            return home.resolve(directory);
        } catch (InvalidPathException unusable) {
            return null;
        }
    }

    private static void filesUnder(Path directory, Path home, Map<String, String> files) throws IOException {
        File[] entries = directory.toFile().listFiles();
        if (entries == null) {
            return;
        }
        for (File entry : entries) {
            if (entry.isDirectory()) {
                filesUnder(entry.toPath(), home, files);
            } else {
                String relative = home.relativize(entry.toPath()).toString().replace('\\', '/');
                files.put(relative, new String(Files.readAllBytes(entry.toPath()), StandardCharsets.UTF_8));
            }
        }
    }

    private static void write(Path target, String text) throws IOException {
        Files.write(target, text.getBytes(StandardCharsets.UTF_8));
    }

    private static JsonElement parse(Path file) throws IOException {
        return JsonParser.parseString(new String(Files.readAllBytes(file), StandardCharsets.UTF_8));
    }

    private static void deleteTree(Path path) throws IOException {
        File file = path.toFile();
        if (!file.exists()) {
            return;
        }
        File[] children = Files.isSymbolicLink(path) ? null : file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child.toPath());
            }
        }
        Files.delete(path);
    }
}
