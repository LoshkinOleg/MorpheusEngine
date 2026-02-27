export interface ModuleUrls {
  intent: string;
  loremaster: string;
  simulator: string;
  arbiter: string;
  proser: string;
}

function getDefaultUrl(role: keyof ModuleUrls): string {
  if (role === "intent") return process.env.MODULE_INTENT_URL ?? "http://localhost:8791";
  if (role === "loremaster") return process.env.MODULE_LOREMASTER_URL ?? "http://localhost:8792";
  if (role === "simulator") return process.env.MODULE_DEFAULT_SIMULATOR_URL ?? "http://localhost:8793";
  if (role === "arbiter") return process.env.MODULE_ARBITER_URL ?? "http://localhost:8795";
  return process.env.MODULE_PROSER_URL ?? "http://localhost:8794";
}

function resolveUrlForBinding(role: keyof ModuleUrls, binding: string | undefined): string {
  if (binding && /^https?:\/\//i.test(binding)) return binding;
  return getDefaultUrl(role);
}

export function resolveModuleUrls(moduleBindings: Record<string, string>): ModuleUrls {
  return {
    intent: resolveUrlForBinding("intent", moduleBindings.intent_extractor),
    loremaster: resolveUrlForBinding("loremaster", moduleBindings.loremaster),
    simulator: resolveUrlForBinding("simulator", moduleBindings.default_simulator),
    arbiter: resolveUrlForBinding("arbiter", moduleBindings.arbiter),
    proser: resolveUrlForBinding("proser", moduleBindings.proser),
  };
}
