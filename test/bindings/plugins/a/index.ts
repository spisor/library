import { appendFileSync, writeFileSync } from "node:fs";

interface Setting {
  readonly value: { readonly kind: number; readonly text: string | null };
}

interface Config {
  get(argument: { readonly key: string }): Promise<Setting | symbol>;
}

interface Context {
  service(id: string): unknown;
  readonly services: { get(service: unknown): unknown; register(service: unknown, object: object): unknown };
}

const textKind = 1;

async function text(config: Config, key: string): Promise<string> {
  const answer = await config.get({ key });
  if (typeof answer === "symbol" || answer.value.kind !== textKind || answer.value.text === null) {
    throw new Error(`a's setting ${key} holds no text`);
  }
  return answer.value.text;
}

export default {
  async activate(context: Context): Promise<void> {
    const config = context.services.get(context.service("weivel.config")) as Config;
    const report = await text(config, "report");
    const greeting = await text(config, "greeting");
    writeFileSync(report, `activated: ${greeting}\n`);
    context.services.register(context.service("a:greeter"), {
      greet(argument: { readonly name: string }): { readonly said: string } {
        appendFileSync(report, `greeted: ${argument.name}\n`);
        return { said: `${greeting}, ${argument.name}` };
      },
    });
  },
};
