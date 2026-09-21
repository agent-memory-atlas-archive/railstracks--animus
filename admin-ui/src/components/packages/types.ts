// Shared types for the API Packages admin section.

export interface PackageRow {
  id: string;
  name: string;
  display_name: string;
  description: string;
  version: string;
  registry_source: string | null;
  registry_version: string | null;
  locally_modified: boolean;
  enabled: boolean;
  keywords: string[];
  _toggling?: boolean;
}

export interface CommandRow {
  id: string;
  name: string;
  kind: string;
  event: string | null;
  description: string;
  parameters: string | null;
  script: string | null;
}

export interface ConnectionRow {
  id: string;
  name: string;
  type: string;
  enabled: boolean;
  poll: number | null;
  hooks: string | null;
}

export interface PackageDetail extends PackageRow {
  state_schema: string;
  state: Record<string, any>;
  // #23: vault status per declared secret key — names + set flag, never values
  secrets?: Record<string, { set: boolean; ref?: string }>;
  commands: CommandRow[];
  connections: ConnectionRow[];
}

// Remote package row as served by the registry browse endpoint (#63).
export interface RegistryRow {
  name: string;
  version: number;
  semantic_version: string;
  description: string;
  content_hash?: string;
  official: boolean;
  published_at: string;
  _installing?: boolean;
  _error?: string;
  _expanded?: boolean;
  _versions?: RegistryVersionRow[];
  _versionsLoading?: boolean;
  _versionsError?: string;
  _versionInstalling?: string;
}

export interface RegistryVersionRow {
  version: number;
  semantic_version: string;
  content_hash: string;
  published_at: string;
}
