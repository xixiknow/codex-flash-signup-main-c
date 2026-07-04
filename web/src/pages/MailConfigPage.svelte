<script lang="ts">
  import { onMount } from 'svelte';
  import PageHeader from '../components/PageHeader.svelte';
  import Panel from '../components/Panel.svelte';
  import EmptyState from '../components/EmptyState.svelte';
  import Modal from '../components/Modal.svelte';
  import Icon from '../components/Icon.svelte';
  import Skeleton from '../components/Skeleton.svelte';
  import { toast } from '../lib/toast';

  type DomainRule = {
    id: number;
    pattern: string;
    base_domain: string;
    wildcard_depth: number;
    is_active: number;
    created_at: number;
    updated_at: number;
  };

  type MailConfig = {
    base_url: string;
    has_api_key: number;
    api_key_preview: string;
    domains: DomainRule[];
  };

  type FetchAction = 'codes' | 'messages' | 'message' | 'message-code';

  const fetchActions: { value: FetchAction; label: string; description: string; needId: boolean; allowLimit: boolean }[] = [
    { value: 'codes', label: '验证码列表', description: '提取最近邮件中的验证码', needId: false, allowLimit: true },
    { value: 'messages', label: '邮件列表', description: '查看完整邮件元信息', needId: false, allowLimit: true },
    { value: 'message', label: '邮件详情', description: '按 Delivery ID 读取', needId: true, allowLimit: false },
    { value: 'message-code', label: '单封验证码', description: '按 Delivery ID 抽取验证码', needId: true, allowLimit: false }
  ];

  let config: MailConfig = $state({
    base_url: '',
    has_api_key: 0,
    api_key_preview: '',
    domains: []
  });
  let initialLoaded = $state(false);

  let configOpen = $state(false);
  let configBaseUrl = $state('');
  let configApiKey = $state('');
  let configClearKey = $state(false);
  let configBusy = $state(false);

  let domainOpen = $state(false);
  let domainPattern = $state('');
  let domainBusy = $state(false);

  let selectedDomainIds: number[] = $state([]);
  let localPart = $state('code');

  let mailbox = $state('');
  let action: FetchAction = $state('codes');
  let deliveryId = $state('');
  let limit = $state(20);
  let fetchBusy = $state(false);
  let fetchResult = $state('');
  let fetchStatus = $state('');
  let fetchEndpoint = $state('');
  let copiedKey: string | null = $state(null);

  let activeAction = $derived(fetchActions.find((a) => a.value === action) ?? fetchActions[0]);
  let parsedFetch = $derived(parseResult(fetchResult));
  let generatedMailboxes = $derived(config.domains.map((domain) => sampleMailbox(domain)));
  let domainPreview = $derived(buildDomainPreview(domainPattern.trim()));

  let fetchStatusOk = $derived(fetchStatus && fetchStatus.startsWith('2'));

  function buildDomainPreview(pattern: string) {
    if (!pattern) return null;
    const wildcards = (pattern.match(/\*/g) ?? []).length;
    const base = pattern.replace(/\*+\./g, '');
    return { base, wildcards };
  }

  async function loadConfig() {
    try {
      const response = await fetch('/api/mail/config');
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      config = await response.json();
      selectedDomainIds = selectedDomainIds.filter((id) => config.domains.some((item) => item.id === id));
      if (!mailbox && config.domains.length > 0) mailbox = sampleMailbox(config.domains[0]);
    } catch (err) {
      toast.error(`配置加载失败：${err instanceof Error ? err.message : String(err)}`);
    } finally {
      initialLoaded = true;
    }
  }

  function openConfigModal() {
    configBaseUrl = config.base_url || 'http://127.0.0.1:8000';
    configApiKey = '';
    configClearKey = false;
    configOpen = true;
  }

  async function submitConfig(event: SubmitEvent) {
    event.preventDefault();
    configBusy = true;
    try {
      const body: { base_url: string; api_key?: string } = { base_url: configBaseUrl.trim() };
      if (configClearKey) body.api_key = '';
      else if (configApiKey.trim() !== '') body.api_key = configApiKey.trim();
      const response = await fetch('/api/mail/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      toast.success(configClearKey ? 'API 密钥已清空' : '配置已保存');
      configOpen = false;
      await loadConfig();
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      configBusy = false;
    }
  }

  function openDomainModal() {
    domainPattern = '';
    domainOpen = true;
  }

  async function submitDomain(event: SubmitEvent) {
    event.preventDefault();
    domainBusy = true;
    try {
      const response = await fetch('/api/mail/domains', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ pattern: domainPattern.trim() })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (!result.ok) throw new Error(result.error || '域名规则无效');
      toast.success(`已保存：${result.pattern}`);
      domainOpen = false;
      domainPattern = '';
      await loadConfig();
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      domainBusy = false;
    }
  }

  async function deleteDomainIds(ids: number[]) {
    if (ids.length === 0) return;
    if (!window.confirm(`确认删除 ${ids.length} 条域名规则？`)) return;
    try {
      const response = await fetch('/api/mail/domains/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ids })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      toast.success(`已删除 ${result.deleted} 条`);
      selectedDomainIds = selectedDomainIds.filter((id) => !ids.includes(id));
      await loadConfig();
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    }
  }

  async function fetchInbox() {
    if (!mailbox.trim()) {
      toast.error('请填写邮箱地址');
      return;
    }
    fetchBusy = true;
    fetchResult = '';
    fetchStatus = '';
    fetchEndpoint = '';
    try {
      const response = await fetch('/api/mail/fetch', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          mailbox: mailbox.trim(),
          action,
          delivery_id: deliveryId.trim(),
          limit
        })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const result = await response.json();
      if (!result.ok) throw new Error(result.error || `HTTP ${result.status_code}`);
      fetchStatus = String(result.status_code ?? '');
      fetchEndpoint = result.endpoint ?? '';
      fetchResult = result.body ?? '';
      toast.success('读取完成');
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      fetchBusy = false;
    }
  }

  function toggleDomain(id: number, event?: Event) {
    event?.stopPropagation();
    selectedDomainIds = selectedDomainIds.includes(id)
      ? selectedDomainIds.filter((item) => item !== id)
      : [...selectedDomainIds, id];
  }

  function toggleAllDomains(event: Event) {
    const checked = (event.currentTarget as HTMLInputElement).checked;
    selectedDomainIds = checked ? config.domains.map((d) => d.id) : [];
  }

  function sampleMailbox(domain: DomainRule) {
    const local = localPart.trim() || 'code';
    const labels = Array.from({ length: domain.wildcard_depth }, (_, index) => `r${index + 1}`);
    const host = [...labels, domain.base_domain].join('.');
    return `${local}@${host}`;
  }

  function fillMailbox(value: string) {
    mailbox = value;
    toast.info(`已填入 ${value}`);
  }

  function parseResult(value: string) {
    if (!value) return null;
    try { return JSON.parse(value); } catch { return null; }
  }

  function prettyResult(value: string) {
    const parsed = parseResult(value);
    if (parsed) return JSON.stringify(parsed, null, 2);
    return value;
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

  onMount(() => {
    loadConfig();
  });
</script>

<PageHeader
  title="邮件"
  subtitle="Rapid-Inbox"
  description="配置外部邮箱接口，管理域名规则，并直接探查任意邮箱的最近内容。"
>
  <button class="btn" type="button" onclick={loadConfig}>
    <Icon name="refresh" size={14} />
    刷新
  </button>
  <button class="btn btn-primary" type="button" onclick={openConfigModal}>
    <Icon name="settings" size={14} />
    API 配置
  </button>
</PageHeader>

<section class="content-grid">
  <Panel title="API 配置" subtitle="Rapid-Inbox">
    {#snippet actions()}
      <button class="btn btn-sm" type="button" onclick={openConfigModal}>
        <Icon name="edit" size={12} />
        编辑
      </button>
    {/snippet}

    {#if !initialLoaded}
      <div style="display: grid; gap: 14px;">
        <Skeleton height="20px" width="60%" />
        <Skeleton height="20px" width="40%" />
        <Skeleton height="20px" width="50%" />
      </div>
    {:else}
      <div class="kv-list">
        <div class="kv-row">
          <span class="kv-key">接口地址</span>
          <span class="kv-val text-mono" style="font-size: 12px; max-width: 60%; overflow: hidden; text-overflow: ellipsis;">{config.base_url || '未配置'}</span>
        </div>
        <div class="kv-row">
          <span class="kv-key">API 密钥</span>
          <span class="kv-val">
            {#if config.has_api_key}
              <span class="pill pill-success">已配置</span>
              <span class="text-mono text-muted" style="margin-left: 8px; font-size: 11px;">{config.api_key_preview}</span>
            {:else}
              <span class="pill">未配置</span>
            {/if}
          </span>
        </div>
        <div class="kv-row">
          <span class="kv-key">域名规则</span>
          <span class="kv-val">{config.domains.length} 条</span>
        </div>
      </div>
    {/if}
  </Panel>

  <Panel title="域名规则" subtitle={`${config.domains.length} 条`}>
    {#snippet actions()}
      <button class="btn btn-sm btn-primary" type="button" onclick={openDomainModal}>
        <Icon name="plus" size={12} />
        添加
      </button>
    {/snippet}

    {#if !initialLoaded}
      <div style="display: grid; gap: 8px;">
        {#each Array(3) as _, i (i)}
          <Skeleton height="48px" rounded="md" />
        {/each}
      </div>
    {:else if config.domains.length === 0}
      <EmptyState
        title="还没有域名规则"
        message="添加规则后即可使用对应域生成临时邮箱"
        icon="tag"
      >
        {#snippet action()}
          <button class="btn btn-sm btn-primary" type="button" onclick={openDomainModal}>
            <Icon name="plus" size={12} />
            添加域名
          </button>
        {/snippet}
      </EmptyState>
    {:else}
      <div style="display: flex; align-items: center; gap: 10px; padding-bottom: 12px; margin-bottom: 12px; border-bottom: 1px solid var(--color-border);">
        <label class="toggle-row" style="font-size: 12px;">
          <input type="checkbox" checked={selectedDomainIds.length === config.domains.length} onchange={toggleAllDomains} />
          <span>全选</span>
        </label>
        {#if selectedDomainIds.length > 0}
          <button class="btn btn-xs btn-danger" type="button" onclick={() => deleteDomainIds(selectedDomainIds)}>
            <Icon name="trash" size={11} />
            删除 ({selectedDomainIds.length})
          </button>
        {/if}
        <span class="flex-spacer"></span>
        <label style="display: flex; align-items: center; gap: 6px; font-size: 11px; color: var(--color-text-muted);">
          本地名
          <input class="input input-sm" style="width: 90px;" bind:value={localPart} />
        </label>
      </div>

      <div style="display: grid; gap: 6px;">
        {#each config.domains as domain, index}
          <div
            class="data-card"
            class:is-selected={selectedDomainIds.includes(domain.id)}
            style="padding: 12px 14px; gap: 6px;"
          >
            <div style="display: flex; align-items: center; gap: 10px;">
              <input
                type="checkbox"
                checked={selectedDomainIds.includes(domain.id)}
                onchange={(event) => toggleDomain(domain.id, event)}
                aria-label={`选择域名 ${domain.id}`}
              />
              <span class="text-mono" style="font-weight: 600; font-size: 13px;">{domain.pattern}</span>
              <span class="tag">深度 {domain.wildcard_depth}</span>
              <span class="flex-spacer"></span>
              <button class="btn btn-xs btn-danger" type="button" onclick={() => deleteDomainIds([domain.id])} aria-label="删除">
                <Icon name="trash" size={11} />
              </button>
            </div>
            <div style="display: flex; align-items: center; gap: 8px; padding-left: 26px;">
              <Icon name="arrow-right" size={11} class="text-muted" />
              <button class="btn-link text-mono" type="button" onclick={() => fillMailbox(generatedMailboxes[index])} style="font-size: 12px;">
                {generatedMailboxes[index]}
              </button>
              <button class="copy-btn" type="button" onclick={() => copy(generatedMailboxes[index], `mb-${domain.id}`, '示例邮箱')} aria-label="复制">
                <Icon name={copiedKey === `mb-${domain.id}` ? 'check' : 'copy'} size={12} />
              </button>
            </div>
          </div>
        {/each}
      </div>
    {/if}
  </Panel>
</section>

<section class="two-col-grid">
  <Panel title="邮箱探查" subtitle="Inbox Fetch">
    <div class="form-section">
      <label class="form-field">
        <span class="form-label">读取类型</span>
        <div class="segmented" style="display: flex; flex-wrap: wrap; max-width: 100%;">
          {#each fetchActions as item}
            <button type="button" class:active={action === item.value} onclick={() => action = item.value}>
              {item.label}
            </button>
          {/each}
        </div>
        <p class="form-help">{activeAction.description}</p>
      </label>

      <label class="form-field">
        <span class="form-label form-label-required">邮箱地址</span>
        <div class="search-input">
          <span class="search-input-icon"><Icon name="mail" size={14} /></span>
          <input class="input" bind:value={mailbox} placeholder="code@example.com" />
        </div>
      </label>

      {#if activeAction.needId}
        <label class="form-field">
          <span class="form-label form-label-required">Delivery ID</span>
          <input class="input text-mono" bind:value={deliveryId} placeholder="DEL_XXXXXXXX" />
        </label>
      {/if}

      {#if activeAction.allowLimit}
        <label class="form-field">
          <span class="form-label">数量上限</span>
          <input class="input" type="number" min="1" max="100" bind:value={limit} />
          <p class="form-help">最多返回 {limit} 条记录</p>
        </label>
      {/if}
    </div>

    <div class="btn-row" style="margin-top: 16px;">
      <button class="btn btn-primary" type="button" onclick={fetchInbox} disabled={fetchBusy || !mailbox.trim()} data-loading={fetchBusy}>
        <Icon name="send" size={14} />
        {fetchBusy ? '读取中…' : '发起读取'}
      </button>
      {#if fetchEndpoint}
        <button class="btn btn-sm" type="button" onclick={() => copy(fetchEndpoint, 'endpoint', '请求 URL')}>
          <Icon name={copiedKey === 'endpoint' ? 'check' : 'link'} size={12} />
          复制 URL
        </button>
      {/if}
    </div>
  </Panel>

  <Panel title="响应" subtitle={fetchStatus ? `HTTP ${fetchStatus}` : 'Result'}>
    {#snippet actions()}
      {#if fetchStatus}
        <span class={fetchStatusOk ? 'pill pill-success' : 'pill pill-danger'}>HTTP {fetchStatus}</span>
      {/if}
      {#if fetchResult}
        <button class="btn btn-sm" type="button" onclick={() => copy(fetchResult, 'body', '响应内容')}>
          <Icon name={copiedKey === 'body' ? 'check' : 'copy'} size={12} />
          复制
        </button>
      {/if}
    {/snippet}

    {#if fetchEndpoint}
      <p class="code-hint">{fetchEndpoint}</p>
    {/if}

    {#if fetchBusy}
      <Skeleton height="160px" rounded="md" />
    {:else if !fetchResult}
      <EmptyState
        title="尚未读取"
        message="选择读取类型并填写邮箱后点击「发起读取」"
        icon="inbox"
      />
    {:else}
      {#if parsedFetch?.items?.length}
        <div class="result-list">
          {#each parsedFetch.items as item}
            <div class="result-item">
              <div style="min-width: 0;">
                <span class="cell-primary text-mono">{item.code ?? item.subject ?? item.delivery_id ?? '-'}</span>
                {#if item.received_at || item.from_addr || item.message_id}
                  <span class="cell-secondary">{item.received_at ?? item.from_addr ?? item.message_id ?? ''}</span>
                {/if}
              </div>
              {#if item.code || item.delivery_id}
                <button class="copy-btn" type="button" onclick={() => copy(item.code ?? item.delivery_id, `r-${item.delivery_id ?? item.code}`)} aria-label="复制">
                  <Icon name={copiedKey === `r-${item.delivery_id ?? item.code}` ? 'check' : 'copy'} size={12} />
                </button>
              {/if}
            </div>
          {/each}
        </div>
      {/if}
      <pre class="code-block">{prettyResult(fetchResult)}</pre>
    {/if}
  </Panel>
</section>

<Modal
  open={configOpen}
  title="API 配置"
  kicker="MAIL · 配置"
  subtitle="Rapid-Inbox 服务地址与认证"
  size="sm"
  onclose={() => { if (!configBusy) configOpen = false; }}
>
  <form id="config-form" class="form-section" onsubmit={submitConfig}>
    <label class="form-field">
      <span class="form-label form-label-required">接口地址</span>
      <div class="search-input">
        <span class="search-input-icon"><Icon name="link" size={14} /></span>
        <input class="input" bind:value={configBaseUrl} placeholder="http://127.0.0.1:8000" required disabled={configBusy} />
      </div>
    </label>
    <label class="form-field">
      <span class="form-label">API 密钥</span>
      <input
        class="input"
        type="password"
        bind:value={configApiKey}
        placeholder={config.has_api_key ? config.api_key_preview : '未配置'}
        disabled={configBusy || configClearKey}
        autocomplete="new-password"
      />
      <p class="form-help">留空则保留现有密钥；勾选下方「清空」以删除已存密钥</p>
    </label>
    {#if config.has_api_key}
      <label class="toggle-row">
        <input type="checkbox" bind:checked={configClearKey} disabled={configBusy} />
        <span>清空已存的 API 密钥</span>
      </label>
    {/if}
  </form>

  {#snippet footer()}
    <button class="btn" type="button" onclick={() => configOpen = false} disabled={configBusy}>取消</button>
    <button class="btn btn-primary" type="submit" form="config-form" disabled={configBusy || !configBaseUrl.trim()} data-loading={configBusy}>
      {configBusy ? '保存中…' : '保存配置'}
    </button>
  {/snippet}
</Modal>

<Modal
  open={domainOpen}
  title="添加域名规则"
  kicker="DOMAIN"
  subtitle="支持精确域名或带通配符的子域规则"
  size="sm"
  onclose={() => { if (!domainBusy) domainOpen = false; }}
>
  <form id="domain-form" class="form-section" onsubmit={submitDomain}>
    <label class="form-field">
      <span class="form-label form-label-required">规则</span>
      <input class="input text-mono" bind:value={domainPattern} placeholder="*.example.com" required disabled={domainBusy} autocomplete="off" />
      <p class="form-help">通配符 <strong>*</strong> 表示任意子域，每个 <strong>*</strong> 代表一层。例如 <code class="text-mono">**.x.com</code> 匹配 <code class="text-mono">a.b.x.com</code>。</p>
    </label>
    {#if domainPreview}
      <div style="display: grid; gap: 8px; padding: 12px 14px; border: 1px solid var(--color-border); border-radius: var(--radius-md); background: var(--color-surface-alt);">
        <div style="display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--color-text-muted);">
          <Icon name="info" size={12} />
          解析结果
        </div>
        <div style="display: flex; gap: 16px; font-size: 12.5px;">
          <span>基础域：<strong class="text-mono">{domainPreview.base}</strong></span>
          <span>通配深度：<strong>{domainPreview.wildcards}</strong></span>
        </div>
      </div>
    {/if}
  </form>

  {#snippet footer()}
    <button class="btn" type="button" onclick={() => domainOpen = false} disabled={domainBusy}>取消</button>
    <button class="btn btn-primary" type="submit" form="domain-form" disabled={domainBusy || !domainPattern.trim()} data-loading={domainBusy}>
      {domainBusy ? '保存中…' : '保存规则'}
    </button>
  {/snippet}
</Modal>
