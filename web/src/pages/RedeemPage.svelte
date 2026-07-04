<script lang="ts">
  import { onMount } from 'svelte';
  import PageHeader from '../components/PageHeader.svelte';
  import Panel from '../components/Panel.svelte';
  import StatusBadge from '../components/StatusBadge.svelte';
  import EmptyState from '../components/EmptyState.svelte';
  import Modal from '../components/Modal.svelte';
  import Icon from '../components/Icon.svelte';
  import Skeleton from '../components/Skeleton.svelte';
  import { toast } from '../lib/toast';

  type RedeemStatus = 'new' | 'redeemed' | 'registered' | 'failed';

  type RedeemItem = {
    id: number;
    code: string;
    status: RedeemStatus;
    email: string;
    product_name: string;
    card_id: number;
    session_id: number;
    end_time: string;
    account_id: number;
    last_error: string;
    created_at: number;
    updated_at: number;
  };

  const statusOptions = [
    { value: '', label: '全部状态' },
    { value: 'new', label: '未兑换' },
    { value: 'redeemed', label: '已兑换' },
    { value: 'registered', label: '已注册' },
    { value: 'failed', label: '失败' }
  ];

  let baseUrl = $state('');
  let items: RedeemItem[] = $state([]);
  let initialLoaded = $state(false);
  let loading = $state(false);
  let actionBusy = $state(false);
  let selectedIds: number[] = $state([]);
  let statusFilter = $state('');
  let limit = $state(50);
  let cursor = $state(0);
  let cursorStack: number[] = $state([]);
  let nextCursor = $state(0);
  let hasMore = $state(false);

  let configOpen = $state(false);
  let configBaseUrl = $state('');
  let configBusy = $state(false);

  let importOpen = $state(false);
  let importText = $state('');
  let importBusy = $state(false);

  let workflow = $state('register_only');
  let concurrency = $state(1);
  let autoUpload = $state(false);

  let allVisibleSelected = $derived(items.length > 0 && items.every((item) => selectedIds.includes(item.id)));

  async function loadConfig() {
    try {
      const response = await fetch('/api/redeem/config');
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      baseUrl = data.base_url ?? '';
    } catch (err) {
      toast.error(`配置加载失败：${err instanceof Error ? err.message : String(err)}`);
    }
  }

  async function loadItems(reset = false) {
    loading = true;
    if (reset) { cursor = 0; cursorStack = []; }
    try {
      const params = new URLSearchParams();
      params.set('limit', String(limit));
      if (cursor > 0) params.set('cursor', String(cursor));
      if (statusFilter) params.set('status', statusFilter);
      const response = await fetch(`/api/redeem?${params.toString()}`);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      items = data.items ?? [];
      hasMore = Boolean(data.has_more);
      nextCursor = Number(data.next_cursor ?? 0);
      selectedIds = selectedIds.filter((id) => items.some((item) => item.id === id));
    } catch (err) {
      items = []; hasMore = false; nextCursor = 0;
      toast.error(`列表加载失败：${err instanceof Error ? err.message : String(err)}`);
    } finally {
      loading = false;
      initialLoaded = true;
    }
  }

  async function refreshAll(reset = false) {
    await loadConfig();
    await loadItems(reset);
  }

  function openConfigModal() {
    configBaseUrl = baseUrl || 'https://sms.paymesh.cn';
    configOpen = true;
  }

  async function submitConfig(event: SubmitEvent) {
    event.preventDefault();
    configBusy = true;
    try {
      const response = await fetch('/api/redeem/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ base_url: configBaseUrl.trim() })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      toast.success('接口地址已保存');
      configOpen = false;
      await loadConfig();
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      configBusy = false;
    }
  }

  function openImportModal() {
    importText = '';
    importOpen = true;
  }

  async function submitImport(event: SubmitEvent) {
    event.preventDefault();
    importBusy = true;
    try {
      const response = await fetch('/api/redeem/import', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text: importText })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (!result.ok) throw new Error(result.error || '导入失败');
      toast.success(`导入完成：新增 ${result.imported}，重复 ${result.skipped}，无效 ${result.invalid}`);
      importOpen = false;
      await loadItems(true);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      importBusy = false;
    }
  }

  async function deleteIds(ids: number[]) {
    if (ids.length === 0) return;
    if (!window.confirm(`确认删除 ${ids.length} 个兑换码？`)) return;
    actionBusy = true;
    try {
      const response = await fetch('/api/redeem/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ids })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      toast.success(`已删除 ${result.deleted} 个`);
      selectedIds = selectedIds.filter((id) => !ids.includes(id));
      await loadItems(false);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      actionBusy = false;
    }
  }

  async function redeemIds(ids: number[]) {
    if (ids.length === 0) return;
    actionBusy = true;
    try {
      const response = await fetch('/api/redeem/redeem', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ids })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      const success = Number(result.success_count ?? 0);
      const failed = Number(result.failed_count ?? 0);
      const firstError = result.details?.find((item: { ok: number; error?: string }) => !item.ok && item.error)?.error;
      const message = `兑换完成：成功 ${success} 个，失败 ${failed} 个${firstError ? `，首个失败：${firstError}` : ''}`;
      if (failed > 0 && success === 0) toast.error(message);
      else if (failed > 0) toast.info(message);
      else toast.success(message);
      await loadItems(false);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      actionBusy = false;
    }
  }

  async function registerIds(ids: number[]) {
    if (ids.length === 0) return;
    actionBusy = true;
    try {
      const response = await fetch('/api/redeem/register', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          ids,
          workflow,
          concurrency: Math.max(1, Math.min(100, Number(concurrency) || 1)),
          auto_upload_oauth_success: autoUpload
        })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (!result.ok) throw new Error(result.error || '任务创建失败');
      toast.success(`兑换注册任务已创建（${result.affected ?? ids.length} 个），可在注册工作台查看进度`);
      await loadItems(false);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      actionBusy = false;
    }
  }

  function toggle(event: Event, id: number) {
    event.stopPropagation();
    selectedIds = selectedIds.includes(id)
      ? selectedIds.filter((item) => item !== id)
      : [...selectedIds, id];
  }

  function toggleAllVisible(event: Event) {
    const checked = (event.currentTarget as HTMLInputElement).checked;
    const visibleIds = items.map((item) => item.id);
    if (checked) selectedIds = Array.from(new Set([...selectedIds, ...visibleIds]));
    else selectedIds = selectedIds.filter((id) => !visibleIds.includes(id));
  }

  function nextPage() {
    if (!hasMore || nextCursor <= 0) return;
    cursorStack = [...cursorStack, cursor];
    cursor = nextCursor;
    loadItems(false);
  }

  function previousPage() {
    if (cursorStack.length === 0) return;
    cursor = cursorStack[cursorStack.length - 1];
    cursorStack = cursorStack.slice(0, -1);
    loadItems(false);
  }

  function statusLabel(status: string) {
    const labels: Record<string, string> = { new: '未兑换', redeemed: '已兑换', registered: '已注册', failed: '失败' };
    return labels[status] ?? status;
  }

  function statusVariant(status: string): 'active' | 'temp' | 'uploaded' | 'failed' | 'default' {
    if (status === 'registered') return 'active';
    if (status === 'redeemed') return 'uploaded';
    if (status === 'failed') return 'failed';
    if (status === 'new') return 'temp';
    return 'default';
  }

  function formatRelativeTime(value: number | undefined | null) {
    if (!value) return '—';
    const ms = value * 1000;
    const diff = Date.now() - ms;
    if (diff < 60_000) return '刚刚';
    if (diff < 3_600_000) return `${Math.floor(diff / 60_000)} 分钟前`;
    if (diff < 86_400_000) return `${Math.floor(diff / 3_600_000)} 小时前`;
    if (diff < 7 * 86_400_000) return `${Math.floor(diff / 86_400_000)} 天前`;
    return new Date(ms).toLocaleDateString();
  }

  onMount(() => { refreshAll(true); });
</script>

<PageHeader
  title="兑换码"
  subtitle="Redeem"
  description="导入并管理 paymesh 兑换码，一键兑换获取邮箱，或直接兑换并注册 GPT 账号。"
>
  <button class="btn" type="button" onclick={() => refreshAll(false)} disabled={loading || actionBusy}>
    <Icon name="refresh" size={14} />
    刷新
  </button>
  <button class="btn" type="button" onclick={openConfigModal}>
    <Icon name="settings" size={14} />
    接口配置
  </button>
  <button class="btn btn-primary" type="button" onclick={openImportModal}>
    <Icon name="plus" size={14} />
    导入兑换码
  </button>
</PageHeader>

<section class="content-grid">
  <Panel title="接口配置" subtitle="paymesh">
    {#snippet actions()}
      <button class="btn btn-sm" type="button" onclick={openConfigModal}>
        <Icon name="edit" size={12} />
        编辑
      </button>
    {/snippet}
    <div class="kv-list">
      <div class="kv-row">
        <span class="kv-key">接口地址</span>
        <span class="kv-val text-mono" style="font-size: 12px; max-width: 60%; overflow: hidden; text-overflow: ellipsis;">{baseUrl || '未配置'}</span>
      </div>
      <div class="kv-row">
        <span class="kv-key">鉴权</span>
        <span class="kv-val"><span class="pill">无需鉴权</span></span>
      </div>
    </div>
  </Panel>

  <Panel title="兑换注册选项" subtitle="Redeem &amp; Register">
    <div class="form-section">
      <label class="form-field">
        <span class="form-label">注册流程</span>
        <select class="input" bind:value={workflow}>
          <option value="register_only">仅注册</option>
          <option value="register_then_oauth">注册后 OAuth</option>
        </select>
      </label>
      <label class="form-field">
        <span class="form-label">并发</span>
        <input class="input" type="number" min="1" max="100" bind:value={concurrency} />
      </label>
      {#if workflow === 'register_then_oauth'}
        <label class="toggle-row">
          <input type="checkbox" bind:checked={autoUpload} />
          <span>OAuth 成功后自动上传到默认 Aether 服务</span>
        </label>
      {/if}
    </div>
  </Panel>
</section>

<Panel title="兑换码列表" subtitle={`${items.length} 条`}>
  <div class="filter-bar accounts-filter-bar">
    <label class="form-field">
      <span class="form-label">状态</span>
      <select class="input" bind:value={statusFilter} onchange={() => loadItems(true)}>
        {#each statusOptions as opt}<option value={opt.value}>{opt.label}</option>{/each}
      </select>
    </label>
    <label class="form-field">
      <span class="form-label">每页</span>
      <select class="input" bind:value={limit} onchange={() => loadItems(true)}>
        <option value={50}>50</option>
        <option value={100}>100</option>
        <option value={200}>200</option>
      </select>
    </label>
    <button class="btn filter-btn" type="button" onclick={() => loadItems(true)} disabled={loading}>
      <Icon name="search" size={12} />
      查询
    </button>
  </div>

  {#if selectedIds.length > 0}
    <div class="bulk-bar">
      <span class="bulk-meta">已选 <strong style="color: var(--color-text); font-weight: 600;">{selectedIds.length}</strong> 个</span>
      <button class="btn btn-sm" type="button" onclick={() => redeemIds(selectedIds)} disabled={loading || actionBusy}>
        <Icon name="tag" size={12} />
        兑换
      </button>
      <button class="btn btn-sm btn-primary" type="button" onclick={() => registerIds(selectedIds)} disabled={loading || actionBusy}>
        <Icon name="zap" size={12} />
        兑换并注册
      </button>
      <button class="btn btn-sm btn-danger" type="button" onclick={() => deleteIds(selectedIds)} disabled={loading || actionBusy}>
        <Icon name="trash" size={12} />
        删除
      </button>
      <button class="btn btn-sm btn-ghost" type="button" onclick={() => selectedIds = []}>取消选择</button>
    </div>
  {/if}

  {#if !initialLoaded}
    <div style="display: grid; gap: 8px;">
      {#each Array(4) as _, i (i)}
        <Skeleton height="48px" rounded="md" />
      {/each}
    </div>
  {:else if items.length === 0}
    <EmptyState
      title="还没有兑换码"
      message="点击「导入兑换码」批量粘贴，每行一个"
      icon="tag"
    >
      {#snippet action()}
        <button class="btn btn-sm btn-primary" type="button" onclick={openImportModal}>
          <Icon name="plus" size={12} />
          导入兑换码
        </button>
      {/snippet}
    </EmptyState>
  {:else}
    <div class="table-wrap has-cards">
      <table class="data-table">
        <thead>
          <tr>
            <th class="col-check"><input type="checkbox" checked={allVisibleSelected} onchange={toggleAllVisible} aria-label="选择当前页" /></th>
            <th>ID</th>
            <th>兑换码</th>
            <th>状态</th>
            <th>邮箱 / 产品</th>
            <th>更新</th>
            <th style="text-align: right;">操作</th>
          </tr>
        </thead>
        <tbody>
          {#each items as item (item.id)}
            <tr class:row-selected={selectedIds.includes(item.id)}>
              <td class="col-check">
                <input type="checkbox" checked={selectedIds.includes(item.id)} onchange={(event) => toggle(event, item.id)} aria-label={`选择 ${item.id}`} />
              </td>
              <td><span class="cell-mono">#{item.id}</span></td>
              <td><span class="cell-primary text-mono">{item.code}</span></td>
              <td><StatusBadge label={statusLabel(item.status)} variant={statusVariant(item.status)} /></td>
              <td>
                {#if item.email}<span class="cell-primary">{item.email}</span>{:else}<span class="cell-secondary">—</span>{/if}
                {#if item.product_name}<span class="cell-secondary">{item.product_name}</span>{/if}
                {#if item.status === 'failed' && item.last_error}<span class="cell-secondary" style="color: var(--color-danger);">{item.last_error}</span>{/if}
              </td>
              <td><span class="cell-primary" style="font-weight: 400;">{formatRelativeTime(item.updated_at)}</span></td>
              <td>
                <div class="inline-actions">
                  <button class="btn btn-xs" type="button" onclick={() => redeemIds([item.id])} disabled={actionBusy} title="兑换">
                    <Icon name="tag" size={11} />
                  </button>
                  <button class="btn btn-xs btn-primary" type="button" onclick={() => registerIds([item.id])} disabled={actionBusy} title="兑换并注册">
                    <Icon name="zap" size={11} />
                  </button>
                  <button class="btn btn-xs btn-danger" type="button" onclick={() => deleteIds([item.id])} disabled={actionBusy} title="删除">
                    <Icon name="trash" size={11} />
                  </button>
                </div>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}

  <div class="pagination-bar">
    <button class="btn btn-sm" type="button" onclick={previousPage} disabled={loading || cursorStack.length === 0}>
      <Icon name="chevron-left" size={12} /> 上一页
    </button>
    <span class="bulk-meta">{loading ? '加载中…' : hasMore ? '还有更多' : '已到末尾'}</span>
    <button class="btn btn-sm" type="button" onclick={nextPage} disabled={loading || !hasMore}>
      下一页 <Icon name="chevron-right" size={12} />
    </button>
  </div>
</Panel>

<Modal
  open={configOpen}
  title="接口配置"
  kicker="REDEEM · 配置"
  subtitle="paymesh 兑换接口地址"
  size="sm"
  onclose={() => { if (!configBusy) configOpen = false; }}
>
  <form id="redeem-config-form" class="form-section" onsubmit={submitConfig}>
    <label class="form-field">
      <span class="form-label form-label-required">接口地址</span>
      <div class="search-input">
        <span class="search-input-icon"><Icon name="link" size={14} /></span>
        <input class="input" bind:value={configBaseUrl} placeholder="https://sms.paymesh.cn" required disabled={configBusy} />
      </div>
      <p class="form-help">兑换调用 <code class="text-mono">{'{地址}'}/api/v1/redeem</code>，验证码轮询 <code class="text-mono">/api/v1/order/lookup</code>。</p>
    </label>
  </form>
  {#snippet footer()}
    <button class="btn" type="button" onclick={() => configOpen = false} disabled={configBusy}>取消</button>
    <button class="btn btn-primary" type="submit" form="redeem-config-form" disabled={configBusy || !configBaseUrl.trim()} data-loading={configBusy}>
      {configBusy ? '保存中…' : '保存配置'}
    </button>
  {/snippet}
</Modal>

<Modal
  open={importOpen}
  title="导入兑换码"
  kicker="REDEEM"
  subtitle="每行一个兑换码，自动去重"
  size="sm"
  onclose={() => { if (!importBusy) importOpen = false; }}
>
  <form id="redeem-import-form" class="form-section" onsubmit={submitImport}>
    <label class="form-field">
      <span class="form-label form-label-required">兑换码</span>
      <textarea class="input text-mono" rows="8" bind:value={importText} placeholder={'66HD-6SV1-WSKG-Q9A8\n....-....-....-....'} required disabled={importBusy}></textarea>
      <p class="form-help">支持形如 <code class="text-mono">XXXX-XXXX-XXXX-XXXX</code> 的兑换码，每行一个。</p>
    </label>
  </form>
  {#snippet footer()}
    <button class="btn" type="button" onclick={() => importOpen = false} disabled={importBusy}>取消</button>
    <button class="btn btn-primary" type="submit" form="redeem-import-form" disabled={importBusy || !importText.trim()} data-loading={importBusy}>
      {importBusy ? '导入中…' : '导入'}
    </button>
  {/snippet}
</Modal>
