import { writeFileSync } from "node:fs";

interface Config {
  get(argument: { readonly key: string }): Promise<{ readonly value: { readonly text: string | null } }>;
}

interface Context {
  service(id: string): unknown;
  readonly services: { get(service: unknown): unknown };
}

export default {
  async activate(context: Context): Promise<void> {
    const config = context.services.get(context.service("weivel.config")) as Config;
    const report = (await config.get({ key: "report" })).value.text;
    if (report !== null) {
      writeFileSync(report, "activated\n");
    }
  },
};
