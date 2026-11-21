import com.weivel.benchmarks.LibraryCallBenchmark;
import com.weivel.benchmarks.PairMeasurement;
import com.weivel.benchmarks.Summary;
import com.weivel.core.Answer;
import com.weivel.core.BoundDefinition;
import com.weivel.core.CoreBinary;
import com.weivel.core.CoreLoad;
import com.weivel.core.DescribedAnswer;
import com.weivel.host.Host;
import com.weivel.host.HostOptions;
import com.weivel.host.LibraryDeclaration;
import com.weivel.host.LoadedLibrary;
import com.weivel.standard.Standard;
import com.weivel.standard.StandardBinary;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * runtime bound from its descriptions by weivel/java's generic binding, as a Java host binds any described C library:
 * weivel.host's start answering its plan of nested structs, weivel.scope opening a plugin's scope whose config is read
 * back, and a description whose member offset disagrees with the platform's layout refused; with the argument
 * {@code cost}, the cost of config.get printed beside the core's echo.
 */
public final class RuntimeBinding {
    private static final String LOADER = "test-host";
    private static final String HOST = "weivel.host";
    private static final String SCOPE = "weivel.scope";
    private static final long SERVE = 1;
    private static final long TEXT = 1;
    private static final long NUMBER = 2;
    /**
     * How config.get is sampled: far fewer calls than the echo benchmark makes, since every call reads the plugin's
     * settings files afresh and so costs a file read where echo costs a copy.
     */
    private static final int WARMUP_CALLS = 200;
    private static final int CALLS_PER_BATCH = 20;
    private static final int ROUNDS = 101;

    private static final StructLayout DESCRIPTION = MemoryLayout.structLayout(
            ValueLayout.JAVA_INT.withName("size"), MemoryLayout.paddingLayout(4),
            ValueLayout.ADDRESS.withName("definition"), ValueLayout.ADDRESS.withName("types"),
            ValueLayout.JAVA_LONG.withName("type_count"), ValueLayout.ADDRESS.withName("enums"),
            ValueLayout.JAVA_LONG.withName("enum_count"), ValueLayout.ADDRESS.withName("operations"),
            ValueLayout.JAVA_LONG.withName("operation_count"), ValueLayout.ADDRESS.withName("blocking"),
            ValueLayout.JAVA_LONG.withName("blocking_count"), ValueLayout.ADDRESS.withName("definitions"),
            ValueLayout.JAVA_LONG.withName("definition_count"));
    private static final StructLayout TYPE = MemoryLayout.structLayout(
            ValueLayout.ADDRESS.withName("name"), ValueLayout.JAVA_LONG.withName("size"),
            ValueLayout.JAVA_LONG.withName("first_shape"), ValueLayout.ADDRESS.withName("members"),
            ValueLayout.JAVA_LONG.withName("member_count"));
    private static final StructLayout MEMBER = MemoryLayout.structLayout(
            ValueLayout.ADDRESS.withName("name"), ValueLayout.JAVA_LONG.withName("offset"),
            ValueLayout.JAVA_LONG.withName("size"), ValueLayout.JAVA_INT.withName("kind"),
            ValueLayout.JAVA_INT.withName("element"), ValueLayout.ADDRESS.withName("type"),
            ValueLayout.ADDRESS.withName("count"), ValueLayout.ADDRESS.withName("release"));

    private static final List<String> failures = new ArrayList<>();

    private RuntimeBinding() {
    }

    /** Runs every check, or with the argument {@code cost} only prints the measurement of config.get. */
    public static void main(String[] arguments) throws Throwable {
        boolean measuring = arguments.length == 1 && arguments[0].equals("cost");
        if (ValueLayout.ADDRESS.byteSize() != Long.BYTES) {
            throw new IllegalStateException("weivel/description.h is laid out here for a 64-bit platform only");
        }
        Path runtime = Path.of(given("SPISOR_BINDINGS_RUNTIME"));
        CoreBinary core = ((CoreLoad.Loaded) CoreBinary.load(Path.of(given("SPISOR_BINDINGS_CORE")))).binary();
        Standard standard = Standard.of(ok(StandardBinary.load(Path.of(given("SPISOR_BINDINGS_STANDARD")))));
        Host host = ok(Host.create(core, HostOptions.defaults().withLog(record -> { })
                .withDescriptionCheck(standard.descriptionCheck())));
        try {
            LibraryDeclaration declaration = standard.readLibrary(
                    Files.readString(runtime.resolveSibling("library.json"))).value().orElseThrow();
            LoadedLibrary library = ok(host.loadLibrary(runtime, declaration));
            BoundDefinition runtimeHost = (BoundDefinition) library.definitions().get(HOST);
            BoundDefinition scopes = (BoundDefinition) library.definitions().get(SCOPE);
            Map<?, ?> plan = (Map<?, ?>) answered(runtimeHost.call("start",
                    Map.of("arguments", List.of(LOADER)))).value();
            planNamesTheInstalledPlugin(plan);
            String manifest = (String) ((Map<?, ?>) ((List<?>) plan.get("plugins")).get(0)).get("manifest");
            DescribedAnswer.Answered opened = answered(scopes.call("open", Map.of("manifest_path", manifest)));
            try {
                BoundDefinition config = (BoundDefinition) ((Map<?, ?>) opened.value()).get("config");
                if (measuring) {
                    measure(core, config);
                } else {
                    configIsReadBack(config);
                }
            } finally {
                opened.release();
            }
            check(Long.valueOf(0).equals(answered(runtimeHost.call("stop", Map.of())).value()), "stop answers 0");
            if (!measuring) {
                movedMemberIsRefused(runtime, declaration);
            }
        } finally {
            host.destroy().toCompletableFuture().get();
            host.close();
        }
        if (!failures.isEmpty()) {
            failures.forEach(failure -> System.err.println("FAILED: " + failure));
            System.exit(1);
        }
        System.out.println("RuntimeBinding: every check passed");
    }

    private static void planNamesTheInstalledPlugin(Map<?, ?> plan) {
        List<?> plugins = (List<?>) plan.get("plugins");
        check(Long.valueOf(SERVE).equals(plan.get("mode")), "a start with only the loader id plans a SERVE run");
        check(plugins.size() == 1, "the plan names the one installed plugin, got " + plugins);
        String manifest = (String) ((Map<?, ?>) plugins.get(0)).get("manifest");
        check(Pattern.compile("[\\\\/]a[\\\\/]1\\.0\\.0[\\\\/]plugin\\.json$").matcher(manifest).find(),
                "the plugin's manifest is its installed plugin.json, got " + manifest);
        check(plan.get("action_plugin") == null, "a SERVE plan names no action plugin");
    }

    private static Map<?, ?> setting(BoundDefinition config, String key) {
        return (Map<?, ?>) ((Map<?, ?>) answered(config.call("get", Map.of("key", key))).value()).get("value");
    }

    private static void configIsReadBack(BoundDefinition config) {
        Map<?, ?> greeting = setting(config, "greeting");
        Map<?, ?> depth = setting(config, "nested.depth");
        check(Long.valueOf(TEXT).equals(greeting.get("kind")), "greeting is a text, got " + greeting);
        check("hello from the manifest's defaults".equals(greeting.get("text")),
                "greeting reads back, got " + greeting);
        check(Long.valueOf(NUMBER).equals(depth.get("kind")) && Double.valueOf(2).equals(depth.get("number")),
                "nested.depth reads back as the number 2, got " + depth);
        List<?> settings = (List<?>) ((Map<?, ?>) answered(config.call("all", Map.of())).value()).get("settings");
        List<Object> keys = new ArrayList<>();
        settings.forEach(each -> keys.add(((Map<?, ?>) each).get("key")));
        check(List.of("greeting", "nested.depth", "report").equals(keys), "all answers every setting, got " + keys);
    }

    private static void movedMemberIsRefused(Path runtime, LibraryDeclaration declaration) throws Throwable {
        List<String> symbols = declaredSymbols(declaration);
        try (Arena arena = Arena.ofConfined()) {
            SymbolLookup lookup = SymbolLookup.libraryLookup(runtime, arena);
            MemorySegment definition = getter(lookup, symbols.get(0));
            MemorySegment description = getter(lookup, symbols.get(1));
            MemorySegment unmoved = withMemberMoved(arena, description, "weivel_standard_host_plan", "plugins", 0);
            MemorySegment moved = withMemberMoved(arena, description, "weivel_standard_host_plan", "plugins", 8);

            check(BoundDefinition.bindDefinition(definition, unmoved).operationNames().contains("start"),
                    "an unmoved copy of weivel.host's description binds");
            try {
                BoundDefinition.bindDefinition(definition, moved);
                check(false, "a description with weivel_standard_host_plan.plugins moved by 8 bytes was bound");
            } catch (IllegalArgumentException refused) {
                check(refused.getMessage().contains("weivel_standard_host_plan.plugins is described at offset"),
                        "the refusal names the member: " + refused.getMessage());
            }
        }
    }

    /** The getter and description symbols the declaration names for weivel.host. */
    private static List<String> declaredSymbols(LibraryDeclaration declaration) {
        LibraryDeclaration.Service service = declaration.services().stream()
                .filter(candidate -> candidate.id().equals(HOST)).findFirst().orElseThrow();
        return List.of(service.functions().get(0).symbol(), service.description().orElseThrow());
    }

    private static MemorySegment getter(SymbolLookup lookup, String symbol) throws Throwable {
        MethodHandle handle = Linker.nativeLinker().downcallHandle(lookup.find(symbol).orElseThrow(),
                FunctionDescriptor.of(ValueLayout.ADDRESS));
        return (MemorySegment) handle.invokeExact();
    }

    /**
     * A copy of the description in which the member of the type is described {@code by} bytes later than the library
     * lays it: the description, its types and that type's members are copied and point at one another.
     */
    private static MemorySegment withMemberMoved(Arena arena, MemorySegment at, String type, String member, long by) {
        MemorySegment head = at.reinterpret(DESCRIPTION.byteSize());
        long typeCount = head.get(ValueLayout.JAVA_LONG, offset(DESCRIPTION, "type_count"));
        MemorySegment types = head.get(ValueLayout.ADDRESS, offset(DESCRIPTION, "types"))
                .reinterpret(typeCount * TYPE.byteSize());
        long typeIndex = indexNamed(types, TYPE, typeCount, type);
        MemorySegment row = types.asSlice(typeIndex * TYPE.byteSize(), TYPE);
        long memberCount = row.get(ValueLayout.JAVA_LONG, offset(TYPE, "member_count"));
        MemorySegment members = row.get(ValueLayout.ADDRESS, offset(TYPE, "members"))
                .reinterpret(memberCount * MEMBER.byteSize());
        long memberIndex = indexNamed(members, MEMBER, memberCount, member);

        MemorySegment movedMembers = arena.allocate(members.byteSize(), Long.BYTES).copyFrom(members);
        long memberOffset = memberIndex * MEMBER.byteSize() + offset(MEMBER, "offset");
        long described = movedMembers.get(ValueLayout.JAVA_LONG, memberOffset);
        movedMembers.set(ValueLayout.JAVA_LONG, memberOffset, described + by);
        MemorySegment movedTypes = arena.allocate(types.byteSize(), Long.BYTES).copyFrom(types);
        movedTypes.set(ValueLayout.ADDRESS, typeIndex * TYPE.byteSize() + offset(TYPE, "members"), movedMembers);
        int size = head.get(ValueLayout.JAVA_INT, offset(DESCRIPTION, "size"));
        MemorySegment movedDescription = arena.allocate(size, Long.BYTES).copyFrom(at.reinterpret(size));
        movedDescription.set(ValueLayout.ADDRESS, offset(DESCRIPTION, "types"), movedTypes);
        return movedDescription;
    }

    private static long indexNamed(MemorySegment rows, StructLayout layout, long count, String name) {
        for (long index = 0; index < count; index++) {
            MemorySegment named = rows.get(ValueLayout.ADDRESS, index * layout.byteSize() + offset(layout, "name"));
            if (name.equals(named.reinterpret(Integer.MAX_VALUE).getString(0))) {
                return index;
            }
        }
        throw new IllegalStateException("the description holds no " + name);
    }

    private static long offset(StructLayout layout, String name) {
        return layout.byteOffset(MemoryLayout.PathElement.groupElement(name));
    }

    /**
     * Prints config.get's median time beside the core's echo, as the binding's own library-call benchmark measures it,
     * so a CI log carries both for its platform; nothing judges them.
     */
    private static void measure(CoreBinary core, BoundDefinition config) {
        Map<String, Object> key = Map.of("key", "greeting");
        for (int call = 0; call < WARMUP_CALLS; call++) {
            config.call("get", key);
        }
        double[] samples = new double[ROUNDS];
        for (int round = 0; round < ROUNDS; round++) {
            long start = System.nanoTime();
            for (int call = 0; call < CALLS_PER_BATCH; call++) {
                config.call("get", key);
            }
            samples[round] = (double) (System.nanoTime() - start) / CALLS_PER_BATCH;
        }
        Summary configGet = Summary.of(samples);
        PairMeasurement echo = LibraryCallBenchmark.measure(core, Path.of(given("SPISOR_BINDINGS_ECHO")),
                LibraryCallBenchmark.DEFAULT_SAMPLING);
        System.out.printf(Locale.ROOT, "spisor-bindings-measurement language=java platform=%s-%s config.get=%.0fns "
                        + "echo=%.0fns bare-echo=%.0fns ratio=%.1f%n", System.getProperty("os.name").replace(' ', '_'),
                System.getProperty("os.arch"), configGet.median(), echo.weivel().median(), echo.bare().median(),
                configGet.median() / echo.weivel().median());
    }

    private static DescribedAnswer.Answered answered(DescribedAnswer answer) {
        if (!(answer instanceof DescribedAnswer.Answered answered)) {
            throw new IllegalStateException("the call did not answer: " + answer);
        }
        return answered;
    }

    private static <T> T ok(Answer<T> answer) {
        if (!(answer instanceof Answer.Ok<T> ok)) {
            throw new IllegalStateException("weivel answered " + answer);
        }
        return ok.value();
    }

    private static String given(String name) {
        String value = System.getenv(name);
        if (value == null || value.isEmpty()) {
            throw new IllegalStateException(name + " is not set; the bindings tests run under ctest, which sets it");
        }
        return value;
    }

    private static void check(boolean holds, String what) {
        if (!holds) {
            failures.add(what);
        }
    }
}
