package com.spisor.library;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonPrimitive;
import io.github.intisy.ai.engine.PluginException;
import io.github.intisy.ai.seam.jvm.library.LibraryBridge;
import io.github.intisy.ai.seam.jvm.library.LibraryDeclaration;
import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

class RuntimeParityTest {

    private static final String IMPLEMENTATION = "java";

    private final ParityCases cases = ParityCases.fromSystemProperty();

    @Test
    void answersEveryCaseAsBasekitDoes() throws Exception {
        List<JsonObject> divergences = divergencesOfThisImplementation();
        List<String> names = cases.names();
        assertFalse(names.isEmpty(), "no cases were found, so the run would hold the library to nothing");
        assertEquals(new ArrayList<String>(), casesMissingFrom(names), "a divergence names a case the run does not have");
        LibraryDeclaration declaration = LibraryDeclaration.parse(new String(Files.readAllBytes(cases.declarationFile()), StandardCharsets.UTF_8));
        List<String> mismatches = new ArrayList<String>();
        Set<JsonObject> held = new HashSet<JsonObject>();
        int calls = 0;

        try (LibraryBridge library = LibraryBridge.open(Paths.get(System.getProperty("parity.binary")), declaration)) {
            for (String name : names) {
                JsonObject expected = cases.expected(name);
                assertEquals(currentPlatform(), expected.get("platform").getAsString(), name + " was answered by basekit on another platform");
                StagedCase staged = cases.stage(name, cases.workDirectory(IMPLEMENTATION));
                assertEquals(staged.calls().size(), expected.getAsJsonArray("answers").size(), name + " has a stale expected file");
                List<JsonElement> answers = observedAnswers(library, declaration, staged);
                calls += answers.size();
                List<JsonObject> ofCase = divergencesOf(divergences, name);
                for (JsonObject entry : ofCase) {
                    if (!entry.has("call") || entry.get("call").getAsInt() < staged.calls().size()) {
                        held.add(entry);
                    }
                }
                mismatches.addAll(answerMismatches(name, answers, expected.getAsJsonArray("answers"), ofCase));
                mismatches.addAll(fileMismatches(name, ParityCases.homeFiles(staged), expected.getAsJsonObject("files"), ofCase));
            }
        }

        assertEquals(cases.declaredCallCount(names), calls, "the run made fewer or more calls than the case files declare");
        List<JsonObject> unheld = new ArrayList<JsonObject>(divergences);
        unheld.removeAll(held);
        System.out.println("java: " + names.size() + " cases, " + calls + " calls, " + held.size()
                + " recorded divergences held, " + mismatches.size() + " mismatches");
        assertEquals(new ArrayList<JsonObject>(), unheld, "a divergence names no call the case makes");
        assertEquals(new ArrayList<String>(), mismatches);
    }

    private List<String> casesMissingFrom(List<String> names) throws IOException {
        List<String> missing = new ArrayList<String>();
        for (JsonElement entry : cases.divergences()) {
            String name = entry.getAsJsonObject().get("case").getAsString();
            if (!names.contains(name)) {
                missing.add(name);
            }
        }
        return missing;
    }

    private List<JsonObject> divergencesOfThisImplementation() throws IOException {
        List<JsonObject> divergences = new ArrayList<JsonObject>();
        for (JsonElement entry : cases.divergences()) {
            JsonObject divergence = entry.getAsJsonObject();
            if (divergence.getAsJsonArray("implementations").contains(new JsonPrimitive(IMPLEMENTATION))) {
                divergences.add(divergence);
            }
        }
        return divergences;
    }

    private static List<JsonObject> divergencesOf(List<JsonObject> divergences, String name) {
        List<JsonObject> ofCase = new ArrayList<JsonObject>();
        for (JsonObject divergence : divergences) {
            if (divergence.get("case").getAsString().equals(name)) {
                ofCase.add(divergence);
            }
        }
        return ofCase;
    }

    private static List<JsonElement> observedAnswers(LibraryBridge library, LibraryDeclaration declaration, StagedCase staged) throws IOException {
        List<JsonElement> answers = new ArrayList<JsonElement>();
        int logIndex = 0;
        for (JsonObject call : staged.calls()) {
            if (ParityCases.isHandEdit(call)) {
                answers.add(ParityCases.performHandEdit(staged, call));
                continue;
            }
            answers.add(observedAnswer(library, declaration, staged, call, logIndex));
            if ("log.write".equals(call.get("function").getAsString())) {
                logIndex += 1;
            }
        }
        return answers;
    }

    private static JsonElement observedAnswer(LibraryBridge library, LibraryDeclaration declaration, StagedCase staged, JsonObject call, int logIndex) {
        String function = call.get("function").getAsString();
        try {
            String answer = library.call(symbolOf(declaration, function), ParityCases.libraryArgument(staged, call, logIndex).toString());
            return ParityCases.normalised(JsonParser.parseString(answer), staged);
        } catch (PluginException refused) {
            if (!refused.getDetail().startsWith(function + ": ")) {
                throw refused;
            }
            return ParityCases.failureAnswer(function, refused.getDetail(), refused.getFix(), staged);
        }
    }

    private static String symbolOf(LibraryDeclaration declaration, String function) {
        String[] parts = function.split("\\.");
        for (LibraryDeclaration.ServiceDeclaration service : declaration.services()) {
            for (LibraryDeclaration.FunctionDeclaration declared : service.functions()) {
                if (service.id().equals(parts[0]) && declared.name().equals(parts[1])) {
                    return declared.symbol();
                }
            }
        }
        throw new IllegalArgumentException("the declaration has no " + function);
    }

    private static List<String> answerMismatches(String name, List<JsonElement> answers, JsonArray basekit, List<JsonObject> divergences) {
        List<String> mismatches = new ArrayList<String>();
        for (int index = 0; index < answers.size(); index++) {
            JsonObject divergence = null;
            for (JsonObject entry : divergences) {
                if (entry.has("call") && entry.get("call").getAsInt() == index) {
                    divergence = entry;
                }
            }
            String at = "call " + index;
            String mismatch = divergence == null
                    ? agreed(name, at, answers.get(index), basekit.get(index))
                    : held(name, at, answers.get(index), basekit.get(index), divergence.get("answer"));
            if (mismatch != null) {
                mismatches.add(mismatch);
            }
        }
        return mismatches;
    }

    private static List<String> fileMismatches(String name, Map<String, String> observed, JsonObject basekit, List<JsonObject> divergences) {
        JsonObject recorded = new JsonObject();
        for (JsonObject entry : divergences) {
            if (entry.has("files")) {
                for (Map.Entry<String, JsonElement> file : entry.getAsJsonObject("files").entrySet()) {
                    recorded.add(file.getKey(), file.getValue());
                }
            }
        }
        Set<String> paths = new TreeSet<String>(observed.keySet());
        paths.addAll(basekit.keySet());
        paths.addAll(recorded.keySet());
        List<String> mismatches = new ArrayList<String>();
        for (String path : paths) {
            JsonElement mine = observed.containsKey(path) ? new JsonPrimitive(observed.get(path)) : JsonNull.INSTANCE;
            JsonElement theirs = basekit.has(path) ? basekit.get(path) : JsonNull.INSTANCE;
            String at = "file " + path;
            String mismatch = recorded.has(path) ? held(name, at, mine, theirs, recorded.get(path)) : agreed(name, at, mine, theirs);
            if (mismatch != null) {
                mismatches.add(mismatch);
            }
        }
        return mismatches;
    }

    private static String agreed(String name, String at, JsonElement observed, JsonElement basekit) {
        return observed.equals(basekit) ? null : describe(name, at, "differs from basekit", observed, basekit);
    }

    /**
     * @implNote A divergence is held in both directions: the library must still answer what the
     * table records, and basekit must still answer something else, so neither side drifts into or
     * out of it unnoticed.
     */
    private static String held(String name, String at, JsonElement observed, JsonElement basekit, JsonElement recorded) {
        if (!observed.equals(recorded)) {
            return describe(name, at, "differs from the recorded divergence", observed, recorded);
        }
        return basekit.equals(recorded) ? describe(name, at, "basekit agrees, so the recorded divergence is stale", observed, basekit) : null;
    }

    private static String describe(String name, String at, String reason, JsonElement observed, JsonElement wanted) {
        return name + " " + at + " " + reason + ": observed " + observed + ", wanted " + wanted;
    }

    private static String currentPlatform() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch").toLowerCase(Locale.ROOT);
        String platform = os.startsWith("windows") ? "win32" : os.startsWith("mac") ? "darwin" : os;
        return platform + "-" + ("amd64".equals(arch) || "x86_64".equals(arch) ? "x64" : arch);
    }
}
