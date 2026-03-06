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

function getBindingEnvOverride(role: keyof ModuleUrls, binding: string): string | null {
  const normalizedBinding = binding
    .trim()
    .replace(/[^a-zA-Z0-9]+/g, "_")
    .toUpperCase();
  if (!normalizedBinding) return null;
  const roleSpecificKey = `MODULE_BINDING_${normalizedBinding}_${role.toUpperCase()}_URL`;
  const genericKey = `MODULE_BINDING_${normalizedBinding}_URL`;
  const roleSpecific = process.env[roleSpecificKey];
  if (typeof roleSpecific === "string" && roleSpecific.trim().length > 0) {
    return roleSpecific.trim();
  }
  const generic = process.env[genericKey];
  if (typeof generic === "string" && generic.trim().length > 0) {
    return generic.trim();
  }
  return null;
}

function resolveUrlForBinding(role: keyof ModuleUrls, binding: string | undefined): string {
  if (binding && /^https?:\/\//i.test(binding)) return binding;
  if (binding) {
    const override = getBindingEnvOverride(role, binding);
    if (override) return override;
  }
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
