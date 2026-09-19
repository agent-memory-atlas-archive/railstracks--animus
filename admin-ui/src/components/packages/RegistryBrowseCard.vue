<template>
  <v-card elevation="2" rounded="lg" class="mb-4">
    <v-card-item>
      <div class="d-flex align-center justify-space-between">
        <v-card-title class="text-subtitle-1">{{ t('packages.availableTitle') }}</v-card-title>
        <v-btn
          size="small"
          variant="text"
          prepend-icon="mdi-refresh"
          :loading="loading"
          :disabled="!registryUrl.trim()"
          @click="browse"
        >
          {{ t('packages.browse') }}
        </v-btn>
      </div>
    </v-card-item>

    <v-card-text>
      <v-text-field
        ref="urlField"
        v-model="registryUrl"
        :label="t('packages.registryLabel')"
        density="comfortable"
        :hint="t('packages.registryHint')"
        persistent-hint
        :disabled="loading"
        class="mb-2"
        @keyup.enter="browse"
      />
    </v-card-text>

    <v-card-text v-if="browsed && !loading && rows.length === 0" class="text-medium-emphasis text-body-2 pb-2">
      {{ source ? t('packages.registryEmpty') : '' }}
    </v-card-text>

    <v-alert
      v-if="browseError"
      type="error"
      variant="tonal"
      density="compact"
      class="mx-4 mb-2"
      :title="t('packages.browseFailed')"
      :text="browseError"
    />

    <v-card-text v-else-if="loading" class="text-center pb-4">
      <v-progress-circular indeterminate color="primary" />
    </v-card-text>

    <v-list v-else-if="rows.length" lines="two" class="pt-0">
      <template v-for="(row, i) in rows" :key="row.name">
        <v-list-item>
          <template #prepend>
            <v-icon :color="statusOf(row).kind === 'installed' ? 'success' : 'primary'">
              mdi-package-variant-closed
            </v-icon>
          </template>

          <v-list-item-title>
            {{ row.name }}
            <v-chip v-if="row.official" size="x-small" color="primary" variant="tonal" class="ml-2">
              <v-icon start size="x-small">mdi-check-decagram</v-icon>
              {{ t('packages.official') }}
            </v-chip>
            <v-chip size="x-small" variant="outlined" class="ml-2">
              v{{ row.semantic_version }}
            </v-chip>
          </v-list-item-title>
          <v-list-item-subtitle>
            {{ row.description }}<span v-if="row.published_at" class="text-disabled"> · {{ fmtDate(row.published_at) }}</span>
          </v-list-item-subtitle>

          <template #append>
            <div class="d-flex align-center gap-2 flex-wrap justify-end" @click.stop>
              <v-chip v-if="source" size="x-small" variant="tonal" color="grey">
                <v-icon start size="x-small">mdi-cloud-outline</v-icon>
                {{ source }}
              </v-chip>

              <v-chip v-if="statusOf(row).kind === 'installed'" size="x-small" color="success" variant="tonal">
                {{ t('packages.installedCurrent', { version: statusOf(row).version }) }}
              </v-chip>
              <v-chip v-else-if="statusOf(row).kind === 'update'" size="x-small" color="warning" variant="tonal">
                {{ t('packages.updateAvailable', { from: statusOf(row).installedVersion, to: statusOf(row).latest }) }}
              </v-chip>
              <v-chip v-else-if="statusOf(row).kind === 'other-source'" size="x-small" color="grey" variant="tonal">
                {{ t('packages.installedOtherSource') }}
              </v-chip>

              <v-btn
                v-if="statusOf(row).kind === 'update'"
                size="small"
                variant="text"
                color="primary"
                :loading="row._installing"
                @click="install(row)"
              >
                {{ t('packages.updateTo', { version: row.semantic_version }) }}
              </v-btn>
              <v-btn
                v-if="statusOf(row).kind !== 'installed' && statusOf(row).kind !== 'update'"
                size="small"
                variant="text"
                color="primary"
                prepend-icon="mdi-download"
                :loading="row._installing"
                @click="install(row)"
              >
                {{ t('packages.install') }}
              </v-btn>

              <v-btn
                size="small"
                variant="text"
                :prepend-icon="row._expanded ? 'mdi-chevron-up' : 'mdi-history'"
                :loading="row._versionsLoading"
                @click="toggleVersions(row)"
              >
                {{ t('packages.versions') }}
              </v-btn>
            </div>
          </template>
        </v-list-item>

        <v-alert
          v-if="row._error"
          type="error"
          variant="tonal"
          density="compact"
          class="mx-4 mb-2"
          :title="t('packages.installFailed')"
          :text="row._error"
        />

        <v-expand-transition>
          <div v-show="row._expanded" class="pb-2">
            <v-alert
              v-if="row._versionsError"
              type="error"
              variant="tonal"
              density="compact"
              class="mx-4 mb-2"
              :text="row._versionsError"
            />
            <v-table v-else-if="row._versions" density="compact" class="mx-4">
              <thead>
                <tr>
                  <th class="text-left">{{ t('packages.versionColumn') }}</th>
                  <th class="text-left">{{ t('packages.published') }}</th>
                  <th class="text-right">{{ t('packages.actions') }}</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="v in row._versions" :key="v.version">
                  <td>
                    v{{ v.semantic_version }}
                    <v-chip v-if="isInstalledVersion(row, v)" size="x-small" color="success" variant="tonal" class="ml-1">
                      {{ t('packages.installedShort') }}
                    </v-chip>
                  </td>
                  <td class="text-medium-emphasis">{{ fmtDate(v.published_at) }}</td>
                  <td class="text-right">
                    <v-btn
                      size="x-small"
                      variant="text"
                      color="primary"
                      :loading="row._versionInstalling === v.semantic_version"
                      :disabled="isInstalledVersion(row, v) || row._installing"
                      @click="install(row, v.semantic_version)"
                    >
                      {{ isInstalledVersion(row, v) ? t('packages.installedShort') : t('packages.installVersion', { version: v.semantic_version }) }}
                    </v-btn>
                  </td>
                </tr>
              </tbody>
            </v-table>
          </div>
        </v-expand-transition>

        <v-divider v-if="i < rows.length - 1" />
      </template>
    </v-list>
  </v-card>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useI18n } from 'vue-i18n';
import type { PackageRow, RegistryRow, RegistryVersionRow } from './types';

const { t } = useI18n();

const props = defineProps<{ installed: PackageRow[] }>();
const emit = defineEmits<{ installed: [] }>();

const DEFAULT_REGISTRY = 'https://animus-registry.steadyfort.com';
const registryUrl = ref(localStorage.getItem('animus.registryUrl') || DEFAULT_REGISTRY);
const rows = ref<RegistryRow[]>([]);
const loading = ref(false);
const browsed = ref(false);
const browseError = ref('');
const source = ref('');
const urlField = ref<{ focus: () => void } | null>(null);

function normalizedRegistry(): string {
  return registryUrl.value.trim().replace(/\/+$/, '');
}

function fmtDate(iso: string): string {
  if (!iso) return '';
  const d = new Date(iso);
  return isNaN(d.getTime()) ? iso : d.toLocaleDateString();
}

function cmpSemver(a: string, b: string): number {
  const pa = (a || '0').split('.').map((n) => parseInt(n, 10) || 0);
  const pb = (b || '0').split('.').map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] || 0) - (pb[i] || 0);
    if (d) return d;
  }
  return 0;
}

type RowStatus =
  | { kind: 'available' }
  | { kind: 'installed'; version: string }
  | { kind: 'update'; installedVersion: string; latest: string }
  | { kind: 'other-source'; version: string };

function statusOf(row: RegistryRow): RowStatus {
  const local = props.installed.find((p) => p.name === row.name);
  if (!local) return { kind: 'available' };
  const localVersion = local.registry_version || local.version || '';
  const sameSource = (local.registry_source || '').replace(/\/+$/, '') === normalizedRegistry();
  if (sameSource) {
    if (localVersion && cmpSemver(localVersion, row.semantic_version) < 0) {
      return { kind: 'update', installedVersion: localVersion, latest: row.semantic_version };
    }
    return { kind: 'installed', version: localVersion || row.semantic_version };
  }
  return { kind: 'other-source', version: localVersion };
}

function isInstalledVersion(row: RegistryRow, v: RegistryVersionRow): boolean {
  const local = props.installed.find((p) => p.name === row.name);
  if (!local) return false;
  const localVersion = local.registry_version || local.version || '';
  return localVersion === v.semantic_version;
}

async function browse() {
  if (!registryUrl.value.trim()) return;
  localStorage.setItem('animus.registryUrl', normalizedRegistry());
  loading.value = true;
  browsed.value = true;
  browseError.value = '';
  try {
    const resp = await fetch(
      `/api/v1/api/registry/packages?registry=${encodeURIComponent(normalizedRegistry())}`
    );
    if (!resp.ok) {
      const err = await resp.json().catch(() => ({}));
      throw new Error(err.error || `HTTP ${resp.status}`);
    }
    const data = await resp.json();
    rows.value = (data.packages || []).map((r: RegistryRow) => ({
      ...r,
      _installing: false,
      _error: '',
      _expanded: false,
      _versionsLoading: false,
      _versionsError: '',
    }));
    source.value = data.source || '';
  } catch (e: any) {
    console.error('Registry browse failed:', e);
    rows.value = [];
    source.value = '';
    browseError.value = e?.message || t('packages.browseFailed');
  } finally {
    loading.value = false;
  }
}

async function toggleVersions(row: RegistryRow) {
  row._expanded = !row._expanded;
  if (row._expanded && !row._versions && !row._versionsLoading) {
    row._versionsLoading = true;
    row._versionsError = '';
    try {
      const resp = await fetch(
        `/api/v1/api/registry/packages/${encodeURIComponent(row.name)}/versions?registry=${encodeURIComponent(normalizedRegistry())}`
      );
      if (!resp.ok) {
        const err = await resp.json().catch(() => ({}));
        throw new Error(err.error || `HTTP ${resp.status}`);
      }
      const data = await resp.json();
      row._versions = data.versions || [];
    } catch (e: any) {
      row._versionsError = e?.message || t('packages.versionsFailed');
    } finally {
      row._versionsLoading = false;
    }
  }
}

async function install(row: RegistryRow, version?: string) {
  row._installing = true;
  if (version) row._versionInstalling = version;
  row._error = '';
  try {
    const resp = await fetch('/api/v1/api/packages/install-from-registry', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        registry: normalizedRegistry(),
        name: row.name,
        version: version || row.semantic_version,
      }),
    });
    if (!resp.ok) {
      const err = await resp.json().catch(() => ({}));
      throw new Error(err.error || `HTTP ${resp.status}`);
    }
    const data = await resp.json();
    emit('installed', data);
  } catch (e: any) {
    // Hash mismatches and lint failures land here — keep them visible inline.
    row._error = e?.message || t('packages.installFailed');
  } finally {
    row._installing = false;
    row._versionInstalling = undefined;
  }
}

function focus() {
  urlField.value?.focus();
}

defineExpose({ focus });
</script>
