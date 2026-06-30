/**
 * Stable entity-id construction. Ids must be deterministic across runs and
 * across producers (TS extractors and the UE commandlet) so that edges from one
 * producer resolve to entities from another. Keep these in lockstep on both sides.
 */

export function assetId(packagePath: string): string {
  return `asset:${packagePath}`;
}

export function bpMemberId(blueprintPackagePath: string, member: string): string {
  return `asset:${blueprintPackagePath}::${member}`;
}

export function cppClassId(name: string): string {
  return `cpp:${name}`;
}

export function cppMemberId(owningType: string, member: string): string {
  return `cpp:${owningType}::${member}`;
}

export function configSectionId(fileRelPath: string, section: string): string {
  return `config:${fileRelPath}#${section}`;
}

export function pluginId(name: string): string {
  return `plugin:${name}`;
}

export function docSectionId(docPath: string, headingPath: string): string {
  return `doc:${docPath}#${headingPath}`;
}
