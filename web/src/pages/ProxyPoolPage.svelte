<script lang="ts">
  import { onMount } from 'svelte';
  import PageHeader from '../components/PageHeader.svelte';
  import Panel from '../components/Panel.svelte';
  import EmptyState from '../components/EmptyState.svelte';
  import Modal from '../components/Modal.svelte';
  import Icon from '../components/Icon.svelte';
  import Skeleton from '../components/Skeleton.svelte';
  import { toast } from '../lib/toast';

  type ProxyScheme = 'http' | 'socks5' | 'socks5h';

  type ProxyItem = {
    id: number;
    scheme: string;
    host: string;
    port: number;
    username: string;
    proxy_url: string;
    status: string;
    last_test_ok: number;
    last_http_status: number;
    exit_ip: string;
    exit_loc: string;
    exit_colo: string;
    trace_http: string;
    trace_tls: string;
    last_error: string;
    last_tested_at: number;
  };

  const schemeOptions: { value: ProxyScheme; label: string; hint: string; defaultPort: number }[] = [
    { value: 'http', label: 'HTTP', hint: '标准 HTTP 代理', defaultPort: 8080 },
    { value: 'socks5', label: 'SOCKS5', hint: '客户端解析 DNS', defaultPort: 1080 },
    { value: 'socks5h', label: 'SOCKS5h', hint: '远端解析 DNS（推荐）', defaultPort: 1080 }
  ];

  const statusFilters = [
    { value: '', label: '全部' },
    { value: 'active', label: '可用' },
    { value: 'failed', label: '失败' },
    { value: 'new', label: '未测试' }
  ];

  let proxies: ProxyItem[] = $state([]);
  let initialLoaded = $state(false);
  let loading = $state(false);
  let testingIds: Set<number> = $state(new Set());
  let busy = $state(false);
  let selectedIds: number[] = $state([]);

  let searchText = $state('');
  let statusFilter = $state('');
  let schemeFilter: string = $state('');

  let addOpen = $state(false);
  let addBusy = $state(false);
  let addScheme: ProxyScheme = $state('socks5h');
  let addHost = $state('');
  let addPort = $state(1080);
  let addUsername = $state('');
  let addPassword = $state('');
  let addTestAfter = $state(true);

  let importOpen = $state(false);
  let importText = $state('');
  let importBusy = $state(false);

  let detailOpen = $state(false);
  let detailProxy: ProxyItem | null = $state(null);
  let copiedKey: string | null = $state(null);

  let filteredProxies = $derived.by(() => {
    return proxies.filter((proxy) => {
      if (statusFilter && proxy.status !== statusFilter) return false;
      if (schemeFilter && proxy.scheme.toLowerCase() !== schemeFilter) return false;
      if (searchText.trim()) {
        const needle = searchText.trim().toLowerCase();
        const haystack = `${proxy.host} ${proxy.port} ${proxy.username} ${proxy.exit_ip} ${proxy.exit_loc} ${proxy.exit_colo}`.toLowerCase();
        if (!haystack.includes(needle)) return false;
      }
      return true;
    });
  });

  let summary = $derived({
    total: proxies.length,
    active: proxies.filter((p) => p.status === 'active').length,
    failed: proxies.filter((p) => p.status === 'failed').length,
    untested: proxies.filter((p) => p.status === 'new' || !p.status).length
  });

  let allVisibleSelected = $derived(
    filteredProxies.length > 0 && filteredProxies.every((p) => selectedIds.includes(p.id))
  );

  let addPreview = $derived(buildProxyUrl(addScheme, addHost.trim(), addPort, addUsername.trim(), addPassword));

  let parsedImportLines = $derived(
    importText
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line.length > 0 && !line.startsWith('#'))
  );

  let hasFilter = $derived(Boolean(searchText.trim() || statusFilter || schemeFilter));

  function buildProxyUrl(scheme: ProxyScheme, host: string, port: number, user: string, pass: string) {
    if (!host || !port) return '';
    const auth = user ? (pass ? `${user}:${pass}@` : `${user}@`) : '';
    return `${scheme}://${auth}${host}:${port}`;
  }

  async function loadProxies(silent = false) {
    if (!silent) loading = true;
    try {
      const response = await fetch('/api/proxies');
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      proxies = data.items ?? [];
      selectedIds = selectedIds.filter((id) => proxies.some((p) => p.id === id));
      if (detailProxy) {
        const fresh = proxies.find((p) => p.id === detailProxy?.id);
        if (fresh) detailProxy = fresh;
      }
    } catch (err) {
      toast.error(`加载失败：${err instanceof Error ? err.message : String(err)}`);
    } finally {
      loading = false;
      initialLoaded = true;
    }
  }

  function resetAddForm() {
    addScheme = 'socks5h';
    addHost = '';
    addPort = 1080;
    addUsername = '';
    addPassword = '';
    addTestAfter = true;
  }

  function openAddModal() {
    resetAddForm();
    addOpen = true;
  }

  function changeScheme(scheme: ProxyScheme) {
    addScheme = scheme;
    const opt = schemeOptions.find((o) => o.value === scheme);
    if (opt && (!addPort || schemeOptions.some((o) => o.defaultPort === addPort))) {
      addPort = opt.defaultPort;
    }
  }

  async function submitAddProxy(event: SubmitEvent) {
    event.preventDefault();
    const url = addPreview;
    if (!url) {
      toast.error('请填写主机和端口');
      return;
    }
    addBusy = true;
    try {
      const response = await fetch('/api/proxies/import', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text: url })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (result.imported > 0) {
        toast.success('代理已添加');
      } else if (result.skipped > 0) {
        toast.info('代理已存在，跳过');
      } else if (result.invalid > 0) {
        throw new Error('代理格式无效');
      }
      addOpen = false;
      await loadProxies(true);
      if (addTestAfter && result.imported > 0) {
        const created = proxies.find(
          (p) => p.host === addHost.trim() && p.port === Number(addPort) && p.scheme.toLowerCase() === addScheme
        );
        if (created) testProxies([created.id]);
      }
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      addBusy = false;
    }
  }

  async function submitBulkImport() {
    if (parsedImportLines.length === 0) {
      toast.error('请粘贴至少一行代理');
      return;
    }
    importBusy = true;
    try {
      const response = await fetch('/api/proxies/import', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text: importText })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      const parts: string[] = [];
      if (result.imported > 0) parts.push(`新增 ${result.imported}`);
      if (result.skipped > 0) parts.push(`重复 ${result.skipped}`);
      if (result.invalid > 0) parts.push(`无效 ${result.invalid}`);
      toast.success(`导入完成：${parts.join(' · ') || '0'}`);
      importText = '';
      importOpen = false;
      await loadProxies(true);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      importBusy = false;
    }
  }

  async function testProxies(ids: number[]) {
    busy = true;
    const targetIds = ids.length > 0 ? ids : proxies.map((p) => p.id);
    targetIds.forEach((id) => testingIds.add(id));
    testingIds = new Set(testingIds);
    try {
      const response = await fetch('/api/proxies/test', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ids })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      const items = result.items ?? [];
      const ok = items.filter((item: { ok: number }) => item.ok).length;
      toast.success(`测试完成：${items.length} 个代理 · ${ok} 可用`);
      await loadProxies(true);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      targetIds.forEach((id) => testingIds.delete(id));
      testingIds = new Set(testingIds);
      busy = false;
    }
  }

  async function deleteIds(ids: number[]) {
    if (ids.length === 0) return;
    if (!window.confirm(`确认删除 ${ids.length} 个代理？此操作不可撤销。`)) return;
    busy = true;
    try {
      const response = await fetch('/api/proxies/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ids })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      toast.success(`已删除 ${result.deleted} 个代理`);
      selectedIds = selectedIds.filter((id) => !ids.includes(id));
      if (detailProxy && ids.includes(detailProxy.id)) {
        detailOpen = false;
        detailProxy = null;
      }
      await loadProxies(true);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      busy = false;
    }
  }

  function toggleSelect(event: Event, id: number) {
    event.stopPropagation();
    selectedIds = selectedIds.includes(id)
      ? selectedIds.filter((item) => item !== id)
      : [...selectedIds, id];
  }

  function toggleAllVisible(event: Event) {
    const checked = (event.currentTarget as HTMLInputElement).checked;
    const visibleIds = filteredProxies.map((p) => p.id);
    if (checked) {
      selectedIds = Array.from(new Set([...selectedIds, ...visibleIds]));
    } else {
      selectedIds = selectedIds.filter((id) => !visibleIds.includes(id));
    }
  }

  function openDetail(proxy: ProxyItem) {
    detailProxy = proxy;
    detailOpen = true;
  }

  function statusLabel(status: string) {
    if (status === 'active') return '可用';
    if (status === 'failed') return '失败';
    return '未测试';
  }

  function statusPillClass(status: string) {
    if (status === 'active') return 'pill pill-success';
    if (status === 'failed') return 'pill pill-danger';
    return 'pill';
  }

  function formatUnixTime(value: number) {
    if (!value) return '从未';
    const ms = value * 1000;
    const diff = Date.now() - ms;
    if (diff < 60_000) return '刚刚';
    if (diff < 3_600_000) return `${Math.floor(diff / 60_000)} 分钟前`;
    if (diff < 86_400_000) return `${Math.floor(diff / 3_600_000)} 小时前`;
    if (diff < 7 * 86_400_000) return `${Math.floor(diff / 86_400_000)} 天前`;
    return new Date(ms).toLocaleDateString();
  }

  function formatExactTime(value: number) {
    if (!value) return '-';
    return new Date(value * 1000).toLocaleString();
  }

  async function copy(value: string, key: string, label = '内容') {
    if (!value) return;
    try {
      await navigator.clipboard.writeText(value);
      copiedKey = key;
      setTimeout(() => { if (copiedKey === key) copiedKey = null; }, 1400);
      toast.success(`${label}已复制`);
    } catch (err) {
      toast.error(`复制失败：${err instanceof Error ? err.message : String(err)}`);
    }
  }

  function clearFilters() {
    searchText = '';
    statusFilter = '';
    schemeFilter = '';
  }

  onMount(() => {
    loadProxies();
  });
</script>

<PageHeader
  title="代理池"
  subtitle="代理管理"
  description="管理 HTTP / SOCKS5 出口节点，支持批量导入、连通性测试与出口探测。"
>
  <button class="btn" type="button" onclick={() => loadProxies()} disabled={loading}>
    <Icon name="refresh" size={14} />
    刷新
  </button>
  <button class="btn" type="button" onclick={() => importOpen = true} disabled={busy}>
    <Icon name="upload" size={14} />
    批量导入
  </button>
  <button class="btn btn-primary" type="button" onclick={openAddModal} disabled={busy}>
    <Icon name="plus" size={14} />
    添加代理
  </button>
</PageHeader>

<section class="stat-strip" aria-label="代理状态汇总">
  <div class="stat-chip">
    <span class="stat-chip-label"><Icon name="globe" size={12} /> 总数</span>
    <span class="stat-chip-value">{summary.total}</span>
    <span class="stat-chip-sub">{filteredProxies.length} 当前可见</span>
  </div>
  <div class="stat-chip stat-chip-success">
    <span class="stat-chip-label"><Icon name="check-circle" size={12} /> 可用</span>
    <span class="stat-chip-value">{summary.active}</span>
    <span class="stat-chip-sub">{summary.total ? Math.round((summary.active / summary.total) * 100) : 0}% 可用率</span>
  </div>
  <div class="stat-chip stat-chip-danger">
    <span class="stat-chip-label"><Icon name="alert-circle" size={12} /> 失败</span>
    <span class="stat-chip-value">{summary.failed}</span>
    <span class="stat-chip-sub">最近测试不通过</span>
  </div>
  <div class="stat-chip">
    <span class="stat-chip-label"><Icon name="clock" size={12} /> 未测试</span>
    <span class="stat-chip-value">{summary.untested}</span>
    <span class="stat-chip-sub">尚未运行连通性</span>
  </div>
</section>

<Panel title="代理列表" subtitle="节点">
  {#snippet actions()}
    <button class="btn btn-sm" type="button" onclick={() => testProxies([])} disabled={busy || proxies.length === 0}>
      <Icon name="zap" size={12} />
      测试全部
    </button>
  {/snippet}

  <div class="filter-bar" style="grid-template-columns: minmax(200px, 1.6fr) minmax(120px, 1fr) minmax(120px, 1fr) auto;">
    <label class="form-field">
      <span class="form-label">搜索</span>
      <div class="search-input">
        <span class="search-input-icon">
          <Icon name="search" size={14} />
        </span>
        <input class="input" bind:value={searchText} placeholder="主机、IP、出口位置" />
        {#if searchText}
          <button class="search-input-clear" type="button" onclick={() => searchText = ''} aria-label="清空搜索">
            <Icon name="close" size={12} />
          </button>
        {/if}
      </div>
    </label>
    <label class="form-field">
      <span class="form-label">状态</span>
      <select class="input" bind:value={statusFilter}>
        {#each statusFilters as opt}<option value={opt.value}>{opt.label}</option>{/each}
      </select>
    </label>
    <label class="form-field">
      <span class="form-label">协议</span>
      <select class="input" bind:value={schemeFilter}>
        <option value="">全部</option>
        <option value="http">HTTP</option>
        <option value="socks5">SOCKS5</option>
        <option value="socks5h">SOCKS5h</option>
      </select>
    </label>
    <button class="btn filter-btn" type="button" onclick={clearFilters} disabled={!hasFilter}>
      清空筛选
    </button>
  </div>

  {#if selectedIds.length > 0}
    <div class="bulk-bar">
      <span class="bulk-meta">已选 <strong style="color: var(--color-text); font-weight: 600;">{selectedIds.length}</strong> 个</span>
      <button class="btn btn-sm btn-primary" type="button" onclick={() => testProxies(selectedIds)} disabled={busy}>
        <Icon name="zap" size={12} />
        测试选中
      </button>
      <button class="btn btn-sm btn-danger" type="button" onclick={() => deleteIds(selectedIds)} disabled={busy}>
        <Icon name="trash" size={12} />
        删除
      </button>
      <button class="btn btn-sm btn-ghost" type="button" onclick={() => selectedIds = []}>
        取消选择
      </button>
    </div>
  {/if}

  {#if !initialLoaded}
    <div class="table-wrap has-cards">
      <table class="data-table">
        <thead>
          <tr>
            <th class="col-check"></th>
            <th>代理地址</th>
            <th>状态</th>
            <th>出口</th>
            <th>协议</th>
            <th>最近测试</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          {#each Array(4) as _, i (i)}
            <tr>
              <td class="col-check"><Skeleton width="16px" height="16px" /></td>
              <td><Skeleton width="180px" height="14px" /><Skeleton width="120px" height="11px" style="margin-top: 6px;" /></td>
              <td><Skeleton width="60px" height="20px" rounded="full" /></td>
              <td><Skeleton width="100px" height="14px" /><Skeleton width="80px" height="11px" style="margin-top: 6px;" /></td>
              <td><Skeleton width="50px" height="14px" /></td>
              <td><Skeleton width="80px" height="12px" /></td>
              <td><Skeleton width="120px" height="24px" /></td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {:else if proxies.length === 0}
    <EmptyState
      title="还没有代理"
      message="添加第一个代理或批量导入一份列表，即可开始测试与分发。"
      icon="globe"
    >
      {#snippet action()}
        <div class="btn-row">
          <button class="btn" type="button" onclick={() => importOpen = true}>
            <Icon name="upload" size={14} />
            批量导入
          </button>
          <button class="btn btn-primary" type="button" onclick={openAddModal}>
            <Icon name="plus" size={14} />
            添加代理
          </button>
        </div>
      {/snippet}
    </EmptyState>
  {:else if filteredProxies.length === 0}
    <EmptyState
      title="没有匹配的代理"
      message="尝试调整搜索关键词或筛选条件。"
      icon="search"
    >
      {#snippet action()}
        <button class="btn btn-sm" type="button" onclick={clearFilters}>
          清空筛选
        </button>
      {/snippet}
    </EmptyState>
  {:else}
    <div class="table-wrap has-cards">
      <table class="data-table">
        <thead>
          <tr>
            <th class="col-check">
              <input type="checkbox" checked={allVisibleSelected} onchange={toggleAllVisible} aria-label="选择全部可见代理" />
            </th>
            <th>代理地址</th>
            <th>状态</th>
            <th>出口信息</th>
            <th>协议指纹</th>
            <th>最近测试</th>
            <th style="text-align: right;">操作</th>
          </tr>
        </thead>
        <tbody>
          {#each filteredProxies as proxy (proxy.id)}
            <tr
              class:row-selected={selectedIds.includes(proxy.id)}
              class="account-row"
              tabindex="0"
              onclick={() => openDetail(proxy)}
              onkeydown={(event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); openDetail(proxy); } }}
            >
              <td class="col-check">
                <input
                  type="checkbox"
                  checked={selectedIds.includes(proxy.id)}
                  onchange={(event) => toggleSelect(event, proxy.id)}
                  onclick={(event) => event.stopPropagation()}
                  aria-label={`选择代理 ${proxy.id}`}
                />
              </td>
              <td>
                <span class="cell-primary text-mono">{proxy.host}<span style="color: var(--color-text-muted);">:</span>{proxy.port}</span>
                <span class="cell-secondary">
                  <span class="tag">{proxy.scheme.toUpperCase()}</span>
                  {#if proxy.username}<span style="margin-left: 6px;">@ {proxy.username}</span>{/if}
                </span>
              </td>
              <td>
                {#if testingIds.has(proxy.id)}
                  <span class="pill"><span class="spinner" style="width: 10px; height: 10px;"></span> 测试中</span>
                {:else}
                  <span class={statusPillClass(proxy.status)}>{statusLabel(proxy.status)}</span>
                {/if}
              </td>
              <td>
                <span class="cell-primary text-mono">{proxy.exit_ip || '—'}</span>
                <span class="cell-secondary">{[proxy.exit_loc, proxy.exit_colo].filter(Boolean).join(' · ') || '—'}</span>
              </td>
              <td>
                <span class="cell-primary">{proxy.trace_http || '—'}</span>
                <span class="cell-secondary">{proxy.trace_tls || '—'}</span>
              </td>
              <td>
                <span class="cell-primary" style="font-weight: 400;">{formatUnixTime(proxy.last_tested_at)}</span>
                {#if proxy.last_http_status}
                  <span class="cell-secondary">HTTP {proxy.last_http_status}</span>
                {/if}
              </td>
              <td>
                <div class="inline-actions">
                  <button class="btn btn-xs" type="button" onclick={(event) => { event.stopPropagation(); testProxies([proxy.id]); }} disabled={busy} aria-label="测试代理">
                    <Icon name="zap" size={11} />
                  </button>
                  <button class="btn btn-xs" type="button" onclick={(event) => { event.stopPropagation(); copy(proxy.proxy_url, `row-${proxy.id}`, '代理 URL'); }} aria-label="复制 URL">
                    <Icon name={copiedKey === `row-${proxy.id}` ? 'check' : 'copy'} size={11} />
                  </button>
                  <button class="btn btn-xs btn-danger" type="button" onclick={(event) => { event.stopPropagation(); deleteIds([proxy.id]); }} disabled={busy} aria-label="删除代理">
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
      {#each filteredProxies as proxy (proxy.id)}
        <div
          class="data-card"
          class:is-selected={selectedIds.includes(proxy.id)}
          role="listitem"
        >
          <input
            class="data-card-checkbox"
            type="checkbox"
            checked={selectedIds.includes(proxy.id)}
            onchange={(event) => toggleSelect(event, proxy.id)}
            aria-label={`选择代理 ${proxy.id}`}
          />
          <div class="data-card-head">
            <button type="button" class="data-card-title text-mono" style="background: transparent; border: 0; padding: 0; cursor: pointer; text-align: left;" onclick={() => openDetail(proxy)}>
              {proxy.host}:{proxy.port}
            </button>
            {#if testingIds.has(proxy.id)}
              <span class="pill"><span class="spinner" style="width: 10px; height: 10px;"></span> 测试中</span>
            {:else}
              <span class={statusPillClass(proxy.status)}>{statusLabel(proxy.status)}</span>
            {/if}
          </div>
          <div class="data-card-meta">
            <span><span class="tag">{proxy.scheme.toUpperCase()}</span>{#if proxy.username}<span style="margin-left: 8px;">@ <strong>{proxy.username}</strong></span>{/if}</span>
            <span>出口 <strong class="text-mono">{proxy.exit_ip || '—'}</strong></span>
            {#if proxy.exit_loc || proxy.exit_colo}<span class="text-muted">{[proxy.exit_loc, proxy.exit_colo].filter(Boolean).join(' · ')}</span>{/if}
            <span class="text-muted">测试 {formatUnixTime(proxy.last_tested_at)}</span>
            {#if proxy.last_error}
              <span class="cell-error" style="font-size: 11.5px;">{proxy.last_error}</span>
            {/if}
          </div>
          <div class="data-card-actions">
            <button class="btn btn-xs" type="button" onclick={() => openDetail(proxy)}>
              <Icon name="info" size={11} />
              详情
            </button>
            <button class="btn btn-xs" type="button" onclick={() => testProxies([proxy.id])} disabled={busy}>
              <Icon name="zap" size={11} />
              测试
            </button>
            <button class="btn btn-xs" type="button" onclick={() => copy(proxy.proxy_url, `card-${proxy.id}`, '代理 URL')}>
              <Icon name={copiedKey === `card-${proxy.id}` ? 'check' : 'copy'} size={11} />
              复制
            </button>
            <button class="btn btn-xs btn-danger" type="button" onclick={() => deleteIds([proxy.id])} disabled={busy}>
              <Icon name="trash" size={11} />
              删除
            </button>
          </div>
        </div>
      {/each}
    </div>
  {/if}
</Panel>

<Modal
  open={addOpen}
  title="添加代理"
  kicker="PROXY · 单条"
  subtitle="填写代理服务器信息，认证字段为可选"
  size="sm"
  onclose={() => { if (!addBusy) addOpen = false; }}
>
  <form id="add-proxy-form" class="form-section" onsubmit={submitAddProxy}>
    <div class="form-field">
      <span class="form-label">代理协议</span>
      <div class="segmented" style="display: flex; width: fit-content;">
        {#each schemeOptions as opt}
          <button type="button" class:active={addScheme === opt.value} onclick={() => changeScheme(opt.value)}>
            {opt.label}
          </button>
        {/each}
      </div>
      <p class="form-help">{schemeOptions.find((o) => o.value === addScheme)?.hint}</p>
    </div>

    <div class="form-grid form-grid-2">
      <label class="form-field" style="grid-column: span 2;">
        <span class="form-label form-label-required">主机 / IP</span>
        <input class="input" bind:value={addHost} placeholder="例如 127.0.0.1 或 example.com" required disabled={addBusy} autocomplete="off" />
      </label>
      <label class="form-field">
        <span class="form-label form-label-required">端口</span>
        <input class="input" type="number" min="1" max="65535" bind:value={addPort} required disabled={addBusy} />
      </label>
    </div>

    <div class="form-grid form-grid-2">
      <label class="form-field">
        <span class="form-label">用户名</span>
        <input class="input" bind:value={addUsername} placeholder="可选" disabled={addBusy} autocomplete="off" />
      </label>
      <label class="form-field">
        <span class="form-label">密码</span>
        <input class="input" type="password" bind:value={addPassword} placeholder="可选" disabled={addBusy} autocomplete="new-password" />
      </label>
    </div>

    <label class="toggle-row">
      <input type="checkbox" bind:checked={addTestAfter} disabled={addBusy} />
      <span>添加后自动测试连通性</span>
    </label>

    {#if addPreview}
      <div style="display: flex; align-items: center; gap: 8px; padding: 10px 12px; border: 1px solid var(--color-border); border-radius: var(--radius-md); background: var(--color-surface-alt); font-family: var(--font-mono); font-size: 12px; color: var(--color-text-secondary); overflow: hidden;">
        <Icon name="link" size={12} />
        <span style="overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">{addPreview.replace(/:[^@/]*@/, ':***@')}</span>
      </div>
    {/if}
  </form>

  {#snippet footer()}
    <button class="btn" type="button" onclick={() => addOpen = false} disabled={addBusy}>取消</button>
    <button class="btn btn-primary" type="submit" form="add-proxy-form" disabled={addBusy || !addPreview} data-loading={addBusy}>
      {addBusy ? '保存中…' : '保存代理'}
    </button>
  {/snippet}
</Modal>

<Modal
  open={importOpen}
  title="批量导入代理"
  kicker="IMPORT"
  subtitle="每行一条，支持 http / socks5 / socks5h，可包含认证"
  size="md"
  onclose={() => { if (!importBusy) importOpen = false; }}
>
  <div class="form-section">
    <label class="form-field">
      <span class="form-label">代理列表</span>
      <textarea
        class="input text-mono"
        rows="10"
        bind:value={importText}
        placeholder={`http://user:pass@127.0.0.1:8080\nsocks5://127.0.0.1:1080\nsocks5h://user:pass@example.com:1080\n# 以 # 开头的行将被忽略`}
        aria-label="批量导入代理"
        disabled={importBusy}
        style="font-size: 12.5px; min-height: 200px;"
      ></textarea>
      <div style="display: flex; align-items: center; justify-content: space-between; gap: 12px;">
        <p class="form-help">
          已识别 <strong style="color: var(--color-text);">{parsedImportLines.length}</strong> 条；空行与注释将被忽略。
        </p>
        {#if parsedImportLines.length > 0}
          <button type="button" class="btn-link" onclick={() => importText = ''} disabled={importBusy}>清空</button>
        {/if}
      </div>
    </label>
  </div>

  {#snippet footer()}
    <button class="btn" type="button" onclick={() => importOpen = false} disabled={importBusy}>取消</button>
    <button class="btn btn-primary" type="button" onclick={submitBulkImport} disabled={importBusy || parsedImportLines.length === 0} data-loading={importBusy}>
      {importBusy ? '导入中…' : `导入 ${parsedImportLines.length} 条`}
    </button>
  {/snippet}
</Modal>

<Modal
  open={detailOpen}
  title="代理详情"
  kicker={detailProxy ? `#${detailProxy.id}` : 'DETAIL'}
  size="md"
  onclose={() => { detailOpen = false; detailProxy = null; }}
>
  {#if detailProxy}
    <div class="account-detail-stack">
      <div class="account-detail-head">
        <div class="account-detail-title-row">
          <h3 class="text-mono" style="font-size: 17px;">{detailProxy.scheme}://{detailProxy.host}:{detailProxy.port}</h3>
          {#if testingIds.has(detailProxy.id)}
            <span class="pill"><span class="spinner" style="width: 10px; height: 10px;"></span> 测试中</span>
          {:else}
            <span class={statusPillClass(detailProxy.status)}>{statusLabel(detailProxy.status)}</span>
          {/if}
        </div>
        {#if detailProxy.last_error}
          <div class="notice-bar notice-error" style="margin: 4px 0 0;">
            <span class="notice-icon"><Icon name="alert-circle" size={14} /></span>
            <span class="notice-text">{detailProxy.last_error}</span>
          </div>
        {/if}
        <div class="account-detail-actions">
          <button class="btn btn-sm" type="button" onclick={() => copy(detailProxy?.proxy_url ?? '', `detail-${detailProxy.id}`, '代理 URL')}>
            <Icon name={copiedKey === `detail-${detailProxy.id}` ? 'check' : 'copy'} size={12} />
            复制完整 URL
          </button>
          <button class="btn btn-sm btn-primary" type="button" onclick={() => testProxies([detailProxy.id])} disabled={busy}>
            <Icon name="zap" size={12} />
            立即测试
          </button>
        </div>
      </div>

      <section class="account-detail-section">
        <h4>连接配置</h4>
        <div class="detail-list">
          <div class="detail-row">
            <span class="detail-label">协议</span>
            <span class="detail-value"><span class="tag">{detailProxy.scheme.toUpperCase()}</span></span>
          </div>
          <div class="detail-row">
            <span class="detail-label">主机</span>
            <span class="detail-value detail-value-code">{detailProxy.host}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">端口</span>
            <span class="detail-value text-mono">{detailProxy.port}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">认证用户</span>
            <span class="detail-value">{detailProxy.username || '—'}</span>
          </div>
        </div>
      </section>

      <section class="account-detail-section">
        <h4>出口探测</h4>
        <div class="detail-list">
          <div class="detail-row">
            <span class="detail-label">出口 IP</span>
            <span class="detail-value detail-value-code">{detailProxy.exit_ip || '—'}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">区域</span>
            <span class="detail-value">{detailProxy.exit_loc || '—'}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">机房</span>
            <span class="detail-value">{detailProxy.exit_colo || '—'}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">HTTP 协议</span>
            <span class="detail-value">{detailProxy.trace_http || '—'}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">TLS</span>
            <span class="detail-value">{detailProxy.trace_tls || '—'}</span>
          </div>
        </div>
      </section>

      <section class="account-detail-section">
        <h4>测试历史</h4>
        <div class="detail-list">
          <div class="detail-row">
            <span class="detail-label">最后测试</span>
            <span class="detail-value">{formatExactTime(detailProxy.last_tested_at)}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">HTTP 状态</span>
            <span class="detail-value">{detailProxy.last_http_status || '—'}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">连通性</span>
            <span class="detail-value">{detailProxy.last_test_ok ? '通过' : '失败'}</span>
          </div>
        </div>
      </section>
    </div>
  {/if}

  {#snippet footer()}
    {#if detailProxy}
      <button class="btn btn-sm btn-danger" type="button" onclick={() => deleteIds([detailProxy.id])} disabled={busy}>
        <Icon name="trash" size={12} />
        删除代理
      </button>
      <span class="flex-spacer"></span>
      <button class="btn btn-sm" type="button" onclick={() => { detailOpen = false; detailProxy = null; }}>关闭</button>
    {/if}
  {/snippet}
</Modal>
