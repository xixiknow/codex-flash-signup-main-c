<script lang="ts">
  import { onMount } from 'svelte';
  import PageHeader from '../components/PageHeader.svelte';
  import Panel from '../components/Panel.svelte';
  import EmptyState from '../components/EmptyState.svelte';
  import Modal from '../components/Modal.svelte';
  import StatusBadge from '../components/StatusBadge.svelte';
  import Icon from '../components/Icon.svelte';
  import Skeleton from '../components/Skeleton.svelte';
  import { toast } from '../lib/toast';

  type AetherStats = {
    account_uploaded: number;
    account_not_uploaded: number;
    total_attempted: number;
    success_count: number;
    failed_count: number;
    skipped_count: number;
    last_success_at: number;
    last_failed_at: number;
    last_message: string;
    updated_at: number;
  };

  type AetherService = {
    id: number;
    name: string;
    api_url: string;
    provider_id: string;
    provider_name: string;
    chatgpt_web_provider_id: string;
    chatgpt_web_provider_name: string;
    proxy_node_id: string;
    proxy_node_name: string;
    has_management_token: number;
    enabled: number;
    priority: number;
    created_at: number;
    updated_at: number;
  };

  type AetherConfig = {
    stats: AetherStats;
    services: AetherService[];
  };

  type ServiceForm = {
    id: number;
    name: string;
    api_url: string;
    management_token: string;
    provider_id: string;
    provider_name: string;
    chatgpt_web_provider_id: string;
    chatgpt_web_provider_name: string;
    proxy_node_id: string;
    proxy_node_name: string;
    enabled: boolean;
    priority: number;
  };

  type AetherPool = {
    provider_id: string;
    provider_name: string;
    provider_type: string;
    total_keys: number;
    active_keys: number;
    cooldown_count: number;
    pool_enabled: number;
  };

  type AetherProxyNode = {
    id: string;
    name: string;
    ip: string;
    port: number;
    region: string;
    status: string;
    is_manual: number;
    tunnel_mode: number;
    tunnel_connected: number;
  };

  const emptyStats: AetherStats = {
    account_uploaded: 0,
    account_not_uploaded: 0,
    total_attempted: 0,
    success_count: 0,
    failed_count: 0,
    skipped_count: 0,
    last_success_at: 0,
    last_failed_at: 0,
    last_message: '',
    updated_at: 0
  };

  let config: AetherConfig = $state({ stats: emptyStats, services: [] });
  let initialLoaded = $state(false);
  let loading = $state(false);
  let busy = $state(false);
  let testingId: number | null = $state(null);
  let formOpen = $state(false);
  let form: ServiceForm = $state(createEmptyForm());
  let optionPools: AetherPool[] = $state([]);
  let optionProxyNodes: AetherProxyNode[] = $state([]);
  let optionsLoading = $state(false);
  let optionsError = $state('');

  let statCards = $derived([
    { label: '已上传账号', value: config.stats.account_uploaded, icon: 'upload' as const, klass: 'stat-chip-success' },
    { label: '未上传账号', value: config.stats.account_not_uploaded, icon: 'database' as const, klass: '' },
    { label: 'Aether 成功', value: config.stats.success_count, icon: 'check-circle' as const, klass: 'stat-chip-info' },
    { label: 'Aether 失败', value: config.stats.failed_count, icon: 'alert-circle' as const, klass: 'stat-chip-danger' },
    { label: '跳过', value: config.stats.skipped_count, icon: 'clock' as const, klass: 'stat-chip-warning' }
  ]);

  function createEmptyForm(): ServiceForm {
    return {
      id: 0,
      name: 'Aether 主服务',
      api_url: '',
      management_token: '',
      provider_id: '',
      provider_name: '',
      chatgpt_web_provider_id: '',
      chatgpt_web_provider_name: '',
      proxy_node_id: '',
      proxy_node_name: '',
      enabled: true,
      priority: 0
    };
  }

  async function loadConfig(silent = false) {
    if (!silent) loading = true;
    try {
      const response = await fetch('/api/upload/aether');
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      config = await response.json();
    } catch (err) {
      toast.error(`上传配置加载失败：${err instanceof Error ? err.message : String(err)}`);
    } finally {
      loading = false;
      initialLoaded = true;
    }
  }

  function openCreate() {
    form = createEmptyForm();
    optionPools = [];
    optionProxyNodes = [];
    optionsError = '';
    formOpen = true;
  }

  function openEdit(service: AetherService) {
    form = {
      id: service.id,
      name: service.name,
      api_url: service.api_url,
      management_token: '',
      provider_id: service.provider_id,
      provider_name: service.provider_name,
      chatgpt_web_provider_id: service.chatgpt_web_provider_id,
      chatgpt_web_provider_name: service.chatgpt_web_provider_name,
      proxy_node_id: service.proxy_node_id,
      proxy_node_name: service.proxy_node_name,
      enabled: Boolean(service.enabled),
      priority: service.priority
    };
    optionPools = [];
    optionProxyNodes = [];
    optionsError = '';
    formOpen = true;
  }

  function closeForm() {
    formOpen = false;
  }

  async function submitForm(event: SubmitEvent) {
    event.preventDefault();
    busy = true;
    try {
      const response = await fetch('/api/upload/aether/service', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(form)
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (!result.ok) throw new Error(result.error || '保存失败');
      toast.success(result.message || 'Aether 上传服务已保存');
      formOpen = false;
      await loadConfig(true);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      busy = false;
    }
  }

  async function testService(payload: Partial<ServiceForm> & { id?: number }) {
    testingId = payload.id ?? 0;
    try {
      const response = await fetch('/api/upload/aether/test', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (!result.ok) throw new Error(result.error || '测试失败');
      if (result.success) toast.success(result.provider_name ? `${result.message}：${result.provider_name}` : result.message);
      else toast.error(result.message || '连接失败');
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      testingId = null;
    }
  }

  async function loadAetherOptions() {
    optionsLoading = true;
    optionsError = '';
    try {
      const response = await fetch('/api/upload/aether/options', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          id: form.id,
          api_url: form.api_url.trim(),
          management_token: form.management_token.trim()
        })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (!result.ok) throw new Error(result.error || '读取选项失败');
      optionPools = result.pools ?? [];
      optionProxyNodes = result.proxy_nodes ?? [];
      optionsError = result.proxy_nodes_error || '';
      const parts = [`Provider ${optionPools.length}`];
      parts.push(`代理节点 ${optionProxyNodes.length}`);
      toast.success(`已读取 ${parts.join(' · ')}`);
      if (optionsError) toast.info(optionsError);
    } catch (err) {
      optionPools = [];
      optionProxyNodes = [];
      optionsError = err instanceof Error ? err.message : String(err);
      toast.error(optionsError);
    } finally {
      optionsLoading = false;
    }
  }

  function selectPool(kind: 'oauth' | 'web', providerId: string) {
    const pool = optionPools.find((item) => item.provider_id === providerId);
    if (kind === 'oauth') {
      form.provider_id = providerId;
      form.provider_name = pool?.provider_name ?? '';
    } else {
      form.chatgpt_web_provider_id = providerId;
      form.chatgpt_web_provider_name = pool?.provider_name ?? '';
    }
  }

  function selectProxyNode(nodeId: string) {
    const node = optionProxyNodes.find((item) => item.id === nodeId);
    form.proxy_node_id = nodeId;
    form.proxy_node_name = node?.name ?? '';
  }

  async function deleteService(service: AetherService) {
    if (!window.confirm(`确认删除 Aether 上传服务「${service.name}」？`)) return;
    busy = true;
    try {
      const response = await fetch('/api/upload/aether/service/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ id: service.id })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (!result.ok) throw new Error(result.error || '删除失败');
      toast.success(result.message || '服务已删除');
      await loadConfig(true);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      busy = false;
    }
  }

  function formatTime(value: number | undefined | null) {
    if (!value) return '—';
    return new Date(value * 1000).toLocaleString();
  }

  function providerDisplay(service: AetherService) {
    return service.provider_name || service.provider_id || '未配置';
  }

  function webProviderDisplay(service: AetherService) {
    if (!service.chatgpt_web_provider_id) return '未配置';
    return service.chatgpt_web_provider_name || service.chatgpt_web_provider_id;
  }

  onMount(() => {
    loadConfig();
  });
</script>

<PageHeader
  title="上传配置"
  subtitle="Aether"
  description="配置账号上传到 Aether 的管理接口，并查看本项目累计上传成功、失败和跳过数量。"
>
  <button class="btn" type="button" onclick={() => loadConfig(false)} disabled={loading || busy}>
    <Icon name="refresh" size={14} />
    刷新
  </button>
  <button class="btn btn-primary" type="button" onclick={openCreate} disabled={busy}>
    <Icon name="plus" size={14} />
    新建配置
  </button>
</PageHeader>

<section class="stat-strip" aria-label="Aether 上传统计">
  {#each statCards as item}
    <div class={`stat-chip ${item.klass}`}>
      <span class="stat-chip-label"><Icon name={item.icon} size={12} /> {item.label}</span>
      <span class="stat-chip-value">{item.value}</span>
    </div>
  {/each}
</section>

<section class="two-col-grid two-col-wide">
  <Panel title="Aether 上传服务" subtitle={`${config.services.length} 个配置`}>
    {#if !initialLoaded}
      <div style="display: grid; gap: 10px;">
        {#each Array(4) as _, index (index)}
          <Skeleton height="48px" rounded="md" />
        {/each}
      </div>
    {:else if config.services.length === 0}
      <EmptyState title="暂无上传配置" message="先添加 Aether 地址、管理员 Token 和 Provider ID" icon="upload">
        {#snippet action()}
          <button class="btn btn-sm btn-primary" type="button" onclick={openCreate}>
            <Icon name="plus" size={12} />
            新建配置
          </button>
        {/snippet}
      </EmptyState>
    {:else}
      <div class="table-wrap has-cards">
        <table class="data-table">
          <thead>
            <tr>
              <th>服务</th>
              <th>Provider</th>
              <th>可选 Web Provider</th>
              <th>状态</th>
              <th style="text-align: right;">操作</th>
            </tr>
          </thead>
          <tbody>
            {#each config.services as service (service.id)}
              <tr>
                <td>
                  <span class="cell-primary">{service.name}</span>
                  <span class="cell-secondary">{service.api_url}</span>
                </td>
                <td>
                  <span class="cell-primary">{providerDisplay(service)}</span>
                  <span class="cell-secondary text-mono">{service.provider_id}</span>
                </td>
                <td>
                  <span class="cell-primary">{webProviderDisplay(service)}</span>
                  {#if service.chatgpt_web_provider_id}
                    <span class="cell-secondary text-mono">{service.chatgpt_web_provider_id}</span>
                  {/if}
                </td>
                <td>
                  <StatusBadge label={service.enabled ? '启用' : '停用'} variant={service.enabled ? 'active' : 'not-uploaded'} />
                  <span class="cell-secondary">优先级 {service.priority}</span>
                </td>
                <td>
                  <div class="inline-actions">
                    <button class="btn btn-xs" type="button" onclick={() => testService({ id: service.id })} disabled={testingId !== null || busy} title="测试连接" aria-label="测试连接">
                      <Icon name={testingId === service.id ? 'refresh' : 'wifi'} size={11} />
                    </button>
                    <button class="btn btn-xs" type="button" onclick={() => openEdit(service)} disabled={busy} title="编辑" aria-label="编辑">
                      <Icon name="edit" size={11} />
                    </button>
                    <button class="btn btn-xs btn-danger" type="button" onclick={() => deleteService(service)} disabled={busy} title="删除" aria-label="删除">
                      <Icon name="trash" size={11} />
                    </button>
                  </div>
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>

      <div class="data-cards" role="list">
        {#each config.services as service (service.id)}
          <div class="data-card" role="listitem">
            <div class="data-card-head">
              <strong class="data-card-title">{service.name}</strong>
              <StatusBadge label={service.enabled ? '启用' : '停用'} variant={service.enabled ? 'active' : 'not-uploaded'} />
            </div>
            <div class="data-card-meta">
              <span>{service.api_url}</span>
              <span>Provider {service.provider_id}</span>
              <span>Web {service.chatgpt_web_provider_id || '未配置'}</span>
            </div>
            <div class="data-card-actions">
              <button class="btn btn-xs" type="button" onclick={() => testService({ id: service.id })} disabled={testingId !== null || busy}>
                <Icon name="wifi" size={11} /> 测试
              </button>
              <button class="btn btn-xs" type="button" onclick={() => openEdit(service)} disabled={busy}>
                <Icon name="edit" size={11} /> 编辑
              </button>
              <button class="btn btn-xs btn-danger" type="button" onclick={() => deleteService(service)} disabled={busy}>
                <Icon name="trash" size={11} /> 删除
              </button>
            </div>
          </div>
        {/each}
      </div>
    {/if}
  </Panel>

  <Panel title="上传状态" subtitle="本地统计">
    <div class="detail-list">
      <div class="detail-row">
        <span class="detail-label">累计尝试</span>
        <span class="detail-value">{config.stats.total_attempted}</span>
      </div>
      <div class="detail-row">
        <span class="detail-label">最近成功</span>
        <span class="detail-value">{formatTime(config.stats.last_success_at)}</span>
      </div>
      <div class="detail-row">
        <span class="detail-label">最近失败</span>
        <span class="detail-value">{formatTime(config.stats.last_failed_at)}</span>
      </div>
      <div class="detail-row">
        <span class="detail-label">最近消息</span>
        <span class="detail-value">{config.stats.last_message || '—'}</span>
      </div>
    </div>
  </Panel>
</section>

<Modal
  open={formOpen}
  title={form.id ? '编辑 Aether 上传服务' : '新建 Aether 上传服务'}
  kicker="AETHER"
  size="lg"
  onclose={closeForm}
>
  <form id="aether-service-form" class="form-grid form-grid-2" onsubmit={submitForm}>
    <label class="form-field">
      <span class="form-label">名称</span>
      <input class="input" bind:value={form.name} required placeholder="Aether 主服务" />
    </label>
    <label class="form-field">
      <span class="form-label">优先级</span>
      <input class="input" type="number" bind:value={form.priority} />
    </label>
    <label class="form-field col-span-full">
      <span class="form-label">Aether 地址</span>
      <input class="input" bind:value={form.api_url} required placeholder="https://aether.example.com" />
    </label>
    <label class="form-field col-span-full">
      <span class="form-label">管理员 Token</span>
      <input class="input" type="password" bind:value={form.management_token} placeholder={form.id ? '留空则沿用已保存 Token' : 'ae_...'} />
    </label>
    <div class="col-span-full btn-row" style="margin: -2px 0 2px;">
      <button class="btn btn-sm" type="button" onclick={loadAetherOptions} disabled={optionsLoading || busy || (!form.id && (!form.api_url.trim() || !form.management_token.trim()))} data-loading={optionsLoading}>
        <Icon name="download" size={12} />
        读取 Provider / 代理节点
      </button>
      {#if optionsError}
        <span class="bulk-meta" style="color: var(--color-danger);">{optionsError}</span>
      {/if}
    </div>
    {#if optionPools.length > 0}
      <label class="form-field">
        <span class="form-label">OAuth 账号池</span>
        <select class="input" bind:value={form.provider_id} onchange={(event) => selectPool('oauth', (event.currentTarget as HTMLSelectElement).value)}>
          <option value="">选择 OAuth 账号池</option>
          {#each optionPools as pool (pool.provider_id)}
            <option value={pool.provider_id}>
              {pool.provider_name || pool.provider_id} · {pool.provider_type || 'provider'} · {pool.active_keys}/{pool.total_keys}
            </option>
          {/each}
        </select>
      </label>
      <label class="form-field">
        <span class="form-label">ChatGPT Web 账号池</span>
        <select class="input" bind:value={form.chatgpt_web_provider_id} onchange={(event) => selectPool('web', (event.currentTarget as HTMLSelectElement).value)}>
          <option value="">不配置 Web 账号池</option>
          {#each optionPools as pool (pool.provider_id)}
            <option value={pool.provider_id}>
              {pool.provider_name || pool.provider_id} · {pool.provider_type || 'provider'} · {pool.active_keys}/{pool.total_keys}
            </option>
          {/each}
        </select>
      </label>
    {/if}
    <label class="form-field">
      <span class="form-label">OAuth Provider ID</span>
      <input class="input" bind:value={form.provider_id} required placeholder="provider-codex" />
    </label>
    <label class="form-field">
      <span class="form-label">OAuth Provider 名称</span>
      <input class="input" bind:value={form.provider_name} placeholder="正常号池" />
    </label>
    <label class="form-field">
      <span class="form-label">ChatGPT Web Provider ID</span>
      <input class="input" bind:value={form.chatgpt_web_provider_id} placeholder="可选" />
    </label>
    <label class="form-field">
      <span class="form-label">ChatGPT Web Provider 名称</span>
      <input class="input" bind:value={form.chatgpt_web_provider_name} placeholder="可选" />
    </label>
    {#if optionProxyNodes.length > 0}
      <label class="form-field col-span-full">
        <span class="form-label">代理节点</span>
        <select class="input" bind:value={form.proxy_node_id} onchange={(event) => selectProxyNode((event.currentTarget as HTMLSelectElement).value)}>
          <option value="">不使用代理节点</option>
          {#each optionProxyNodes as node (node.id)}
            <option value={node.id}>
              {node.name || node.id} · {node.region || node.status || 'online'} · {node.ip}{node.port ? `:${node.port}` : ''}
            </option>
          {/each}
        </select>
      </label>
    {/if}
    <label class="form-field">
      <span class="form-label">代理节点 ID</span>
      <input class="input" bind:value={form.proxy_node_id} placeholder="可选" />
    </label>
    <label class="form-field">
      <span class="form-label">代理节点名称</span>
      <input class="input" bind:value={form.proxy_node_name} placeholder="可选" />
    </label>
    <label class="form-field form-field-inline col-span-full">
      <input type="checkbox" bind:checked={form.enabled} />
      <span>启用此上传服务</span>
    </label>
  </form>

  {#snippet footer()}
    <button class="btn btn-sm" type="button" onclick={loadAetherOptions} disabled={optionsLoading || busy || (!form.id && (!form.api_url.trim() || !form.management_token.trim()))} data-loading={optionsLoading}>
      <Icon name="download" size={12} />
      读取选项
    </button>
    <button class="btn btn-sm" type="button" onclick={() => testService(form)} disabled={testingId !== null || busy}>
      <Icon name="wifi" size={12} />
      测试连接
    </button>
    <span class="flex-spacer"></span>
    <button class="btn btn-sm" type="button" onclick={closeForm} disabled={busy}>取消</button>
    <button class="btn btn-sm btn-primary" type="submit" form="aether-service-form" disabled={busy} data-loading={busy}>
      <Icon name="check" size={12} />
      保存
    </button>
  {/snippet}
</Modal>
