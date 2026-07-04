<script lang="ts">
  import { onMount } from 'svelte';
  import PageHeader from '../components/PageHeader.svelte';
  import Panel from '../components/Panel.svelte';
  import Icon from '../components/Icon.svelte';
  import Skeleton from '../components/Skeleton.svelte';
  import { toast } from '../lib/toast';

  type Variant = 'default' | 'success' | 'warning' | 'danger';

  type DiskStatus = {
    name: string; path: string; total_bytes: number; used_bytes: number;
    free_bytes: number; usage_pct: number;
  };

  type SystemStatus = {
    status: string; uptime_ms: number; connections: number; server: string; sampled_at_ms: number;
    cpu: { cores: number; usage_pct: number; load1: number; load5: number; load15: number; };
    memory: {
      total_bytes: number; used_bytes: number; free_bytes: number;
      shared_bytes: number; buff_cache_bytes: number; available_bytes: number;
      usage_pct: number; swap_total_bytes: number; swap_used_bytes: number;
      swap_free_bytes: number; swap_usage_pct: number;
    };
    storage: { root: DiskStatus; data: DiskStatus; };
    network: { rx_bytes: number; tx_bytes: number; rx_rate_Bps: number; tx_rate_Bps: number; };
    sqlite: {
      path: string; size_bytes: number; wal_size_bytes: number; shm_size_bytes: number;
      page_size: number; page_count: number; freelist_count: number;
      estimated_size_bytes: number; freelist_pct: number; journal_mode: string;
      synchronous: number; wal_autocheckpoint: number; cache_size: number;
      foreign_keys: number; schema_version: number; user_version: number;
      tables_count: number; indexes_count: number; triggers_count: number;
      accounts_total: number; accounts_active: number; accounts_expired: number;
      accounts_temp: number; accounts_failed: number; accounts_uploaded: number;
      accounts_not_uploaded: number; stats_updated_at: number;
    };
  };

  let status: SystemStatus | null = $state(null);
  let loading = $state(false);
  let wsState = $state<'connecting' | 'open' | 'closed' | 'error'>('closed');
  let refreshIntervalMs = $state(3000);
  let lastReceivedAt: Date | null = $state(null);
  let socket: WebSocket | null = null;
  let reconnectTimer: number | null = null;
  let reconnectDelayMs = 1000;
  let shouldReconnect = true;

  function formatUptime(ms: number | undefined) {
    if (ms === undefined) return '—';
    const total = Math.floor(ms / 1000);
    const days = Math.floor(total / 86400);
    const hours = Math.floor((total % 86400) / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const seconds = total % 60;
    if (days > 0) return `${days}天 ${hours}时`;
    if (hours > 0) return `${hours}时 ${minutes}分`;
    if (minutes > 0) return `${minutes}分 ${seconds}秒`;
    return `${seconds}秒`;
  }

  function formatBytes(value: number | undefined | null) {
    if (value === undefined || value === null) return '—';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let size = value;
    let unit = 0;
    while (size >= 1024 && unit < units.length - 1) {
      size /= 1024;
      unit += 1;
    }
    if (unit === 0) return `${Math.round(size)} ${units[unit]}`;
    return `${size.toFixed(size >= 10 ? 1 : 2)} ${units[unit]}`;
  }

  function formatRate(value: number | undefined | null) {
    if (value === undefined || value === null) return '—';
    return `${formatBytes(value)}/s`;
  }

  function formatPercent(value: number | undefined | null) {
    if (value === undefined || value === null) return '—';
    return `${value.toFixed(1)}%`;
  }

  function formatTime(value: Date | null) {
    return value ? value.toLocaleTimeString() : '—';
  }

  function formatUnixTime(seconds: number | undefined | null) {
    if (!seconds) return '—';
    return new Date(seconds * 1000).toLocaleString();
  }

  function displayWsState(value: string) {
    const states: Record<string, string> = { connecting: '连接中', open: '已连接', closed: '重连中', error: '异常' };
    return states[value] ?? value;
  }

  function sqliteSynchronousLabel(value: number | undefined | null) {
    const labels: Record<number, string> = { 0: 'OFF', 1: 'NORMAL', 2: 'FULL', 3: 'EXTRA' };
    if (value === undefined || value === null) return '—';
    return labels[value] ?? String(value);
  }

  function sqliteCacheSizeLabel(value: number | undefined | null) {
    if (value === undefined || value === null) return '—';
    if (value < 0) return `${Math.abs(value)} KiB`;
    return `${value} 页`;
  }

  function enabledLabel(value: number | undefined | null) {
    if (value === undefined || value === null) return '—';
    return value ? '开启' : '关闭';
  }

  function usageVariant(value: number | undefined | null): Variant {
    if (value === undefined || value === null) return 'default';
    if (value >= 90) return 'danger';
    if (value >= 75) return 'warning';
    return 'success';
  }

  function wsChipClass() {
    if (wsState === 'open') return 'stat-chip stat-chip-success';
    if (wsState === 'connecting' || wsState === 'closed') return 'stat-chip stat-chip-warning';
    return 'stat-chip stat-chip-danger';
  }

  function wsIcon() {
    if (wsState === 'open') return 'wifi';
    if (wsState === 'error') return 'wifi-off';
    return 'wifi';
  }

  function barWidth(value: number | undefined | null) {
    if (value === undefined || value === null || Number.isNaN(value)) return 0;
    return Math.min(100, Math.max(0, value));
  }

  function applyStatus(next: SystemStatus) {
    status = next;
    lastReceivedAt = new Date();
    loading = false;
  }

  async function refreshStatus() {
    loading = true;
    try {
      const response = await fetch('/api/status');
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      applyStatus(await response.json());
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      loading = false;
    }
  }

  function subscribeSystemStatus() {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    socket.send(JSON.stringify({ type: 'system_subscribe', interval_ms: refreshIntervalMs }));
  }

  function scheduleReconnect() {
    if (!shouldReconnect || reconnectTimer !== null) return;
    reconnectTimer = window.setTimeout(() => {
      reconnectTimer = null;
      connectWebSocket();
    }, reconnectDelayMs);
    reconnectDelayMs = Math.min(reconnectDelayMs * 2, 10000);
  }

  function connectWebSocket() {
    if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) {
      return;
    }
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
    socket = new WebSocket(`${scheme}://${location.host}/ws`);
    wsState = 'connecting';

    socket.onopen = () => {
      wsState = 'open';
      reconnectDelayMs = 1000;
      subscribeSystemStatus();
    };

    socket.onmessage = (event) => {
      try {
        const message = JSON.parse(event.data);
        if (message.type === 'system_status' && message.payload) {
          applyStatus(message.payload);
        }
      } catch (err) {
        console.warn('system ws parse failed', err);
      }
    };

    socket.onerror = () => { wsState = 'error'; };

    socket.onclose = () => {
      wsState = 'closed';
      socket = null;
      scheduleReconnect();
    };
  }

  function handleIntervalChange() {
    subscribeSystemStatus();
  }

  onMount(() => {
    shouldReconnect = true;
    refreshStatus();
    connectWebSocket();
    return () => {
      shouldReconnect = false;
      if (reconnectTimer !== null) window.clearTimeout(reconnectTimer);
      if (socket) socket.close();
    };
  });
</script>

<PageHeader
  title="运行仪表盘"
  subtitle="控制台"
  description="实时监控系统资源、网络吞吐与 SQLite 状态。"
>
  <select class="input input-sm refresh-select" aria-label="刷新间隔" bind:value={refreshIntervalMs} onchange={handleIntervalChange}>
    <option value={1000}>每 1 秒</option>
    <option value={3000}>每 3 秒</option>
    <option value={10000}>每 10 秒</option>
    <option value={30000}>每 30 秒</option>
  </select>
  <button class="btn btn-primary" type="button" onclick={refreshStatus} disabled={loading} data-loading={loading}>
    <Icon name="refresh" size={14} />
    {loading ? '刷新中…' : '立即刷新'}
  </button>
</PageHeader>

<section class="stat-strip">
  <div class={`stat-chip ${status?.status === 'ok' ? 'stat-chip-success' : ''}`}>
    <span class="stat-chip-label"><Icon name="activity" size={12} /> 服务状态</span>
    <span class="stat-chip-value" style="font-size: 18px;">{status?.status === 'ok' ? '正常' : '—'}</span>
    <span class="stat-chip-sub">{status?.server || 'mongoose'}</span>
  </div>
  <div class="stat-chip">
    <span class="stat-chip-label"><Icon name="clock" size={12} /> 运行时长</span>
    <span class="stat-chip-value" style="font-size: 18px;">{formatUptime(status?.uptime_ms)}</span>
  </div>
  <div class="stat-chip">
    <span class="stat-chip-label"><Icon name="link" size={12} /> 活跃连接</span>
    <span class="stat-chip-value">{status?.connections ?? '—'}</span>
  </div>
  <div class={wsChipClass()}>
    <span class="stat-chip-label"><Icon name={wsIcon()} size={12} /> 监控通道</span>
    <span class="stat-chip-value" style="font-size: 16px;">{displayWsState(wsState)}</span>
    <span class="stat-chip-sub">最近刷新 {formatTime(lastReceivedAt)}</span>
  </div>
</section>

<section class="dash-grid">
  <div class="dash-main">
    <Panel title="主机资源" subtitle="实时">
      {#if !status}
        <div style="display: grid; gap: 22px;">
          {#each Array(4) as _, i (i)}
            <div>
              <div style="display: flex; justify-content: space-between; margin-bottom: 8px;">
                <Skeleton width="60px" height="12px" />
                <Skeleton width="40px" height="14px" />
              </div>
              <Skeleton height="6px" rounded="full" />
              <Skeleton width="70%" height="11px" style="margin-top: 8px;" />
            </div>
          {/each}
        </div>
      {:else}
        {@const memTotal = status.memory.total_bytes || 1}
        <div class="gauge-list">
          <div class="gauge-item">
            <div class="gauge-header">
              <span class="gauge-label"><Icon name="cpu" size={11} class="text-muted" /> CPU</span>
              <span class="gauge-pct" class:text-warning={usageVariant(status.cpu.usage_pct) === 'warning'} class:text-danger={usageVariant(status.cpu.usage_pct) === 'danger'}>{formatPercent(status.cpu.usage_pct)}</span>
            </div>
            <div class="gauge-bar">
              <span class="gauge-fill" class:gauge-fill-warning={usageVariant(status.cpu.usage_pct) === 'warning'} class:gauge-fill-danger={usageVariant(status.cpu.usage_pct) === 'danger'} style={`width: ${barWidth(status.cpu.usage_pct)}%`}></span>
            </div>
            <span class="gauge-detail">{status.cpu.cores} 核 · 负载 {status.cpu.load1.toFixed(2)} / {status.cpu.load5.toFixed(2)} / {status.cpu.load15.toFixed(2)}</span>
          </div>

          <div class="gauge-item">
            <div class="gauge-header">
              <span class="gauge-label"><Icon name="hard-drive" size={11} class="text-muted" /> 内存</span>
              <span class="gauge-pct" class:text-warning={usageVariant(status.memory.usage_pct) === 'warning'} class:text-danger={usageVariant(status.memory.usage_pct) === 'danger'}>{formatPercent(status.memory.usage_pct)}</span>
            </div>
            <div class="seg-bar">
              <div class="seg-track">
                <span class="seg seg-used" style={`width: ${((status.memory.used_bytes - status.memory.buff_cache_bytes - status.memory.shared_bytes) / memTotal * 100)}%`}></span>
                <span class="seg seg-shared" style={`width: ${(status.memory.shared_bytes / memTotal * 100)}%`}></span>
                <span class="seg seg-cache" style={`width: ${(status.memory.buff_cache_bytes / memTotal * 100)}%`}></span>
              </div>
              <div class="seg-tooltip">
                <div class="seg-tip-row"><span class="seg-dot seg-dot-used"></span><span>已用</span><strong>{formatBytes(status.memory.used_bytes - status.memory.buff_cache_bytes - status.memory.shared_bytes)}</strong></div>
                <div class="seg-tip-row"><span class="seg-dot seg-dot-shared"></span><span>共享</span><strong>{formatBytes(status.memory.shared_bytes)}</strong></div>
                <div class="seg-tip-row"><span class="seg-dot seg-dot-cache"></span><span>缓存</span><strong>{formatBytes(status.memory.buff_cache_bytes)}</strong></div>
                <div class="seg-tip-row"><span class="seg-dot seg-dot-free"></span><span>可用</span><strong>{formatBytes(status.memory.available_bytes)}</strong></div>
                <div class="seg-tip-row seg-tip-total"><span></span><span>总计</span><strong>{formatBytes(status.memory.total_bytes)}</strong></div>
              </div>
            </div>
            <span class="gauge-detail">{formatBytes(status.memory.total_bytes)} 总计 · {formatBytes(status.memory.available_bytes)} 可用</span>
            {#if status.memory.swap_total_bytes > 0}
              <span class="gauge-detail gauge-detail-sub">Swap {formatPercent(status.memory.swap_usage_pct)} — {formatBytes(status.memory.swap_used_bytes)} / {formatBytes(status.memory.swap_total_bytes)}</span>
            {/if}
          </div>

          <div class="gauge-item">
            <div class="gauge-header">
              <span class="gauge-label"><Icon name="database" size={11} class="text-muted" /> 数据盘</span>
              <span class="gauge-pct" class:text-warning={usageVariant(status.storage.data.usage_pct) === 'warning'} class:text-danger={usageVariant(status.storage.data.usage_pct) === 'danger'}>{formatPercent(status.storage.data.usage_pct)}</span>
            </div>
            <div class="gauge-bar">
              <span class="gauge-fill" class:gauge-fill-warning={usageVariant(status.storage.data.usage_pct) === 'warning'} class:gauge-fill-danger={usageVariant(status.storage.data.usage_pct) === 'danger'} style={`width: ${barWidth(status.storage.data.usage_pct)}%`}></span>
            </div>
            <span class="gauge-detail">已用 {formatBytes(status.storage.data.used_bytes)} / 总计 {formatBytes(status.storage.data.total_bytes)} · 根分区 {formatPercent(status.storage.root.usage_pct)}</span>
          </div>

          <div class="gauge-item gauge-item-no-bar">
            <div class="gauge-header">
              <span class="gauge-label"><Icon name="activity" size={11} class="text-muted" /> 网络</span>
              <span class="gauge-pct text-mono" style="font-size: 13px;">↓ {formatRate(status.network.rx_rate_Bps)}  ↑ {formatRate(status.network.tx_rate_Bps)}</span>
            </div>
            <span class="gauge-detail">累计 ↓ {formatBytes(status.network.rx_bytes)} · ↑ {formatBytes(status.network.tx_bytes)}</span>
          </div>
        </div>
      {/if}
    </Panel>

    <Panel title="账号概览" subtitle="缓存统计">
      {#if !status}
        <div class="mini-stats mini-stats-wide">
          {#each Array(7) as _, i (i)}
            <Skeleton height="68px" rounded="md" />
          {/each}
        </div>
      {:else}
        <div class="mini-stats mini-stats-wide">
          <div class="mini-stat">
            <span class="mini-stat-val">{status.sqlite.accounts_total ?? '—'}</span>
            <span class="mini-stat-label">总量</span>
          </div>
          <div class="mini-stat mini-stat-success">
            <span class="mini-stat-val">{status.sqlite.accounts_active ?? '—'}</span>
            <span class="mini-stat-label">活跃</span>
          </div>
          <div class="mini-stat mini-stat-warning">
            <span class="mini-stat-val">{status.sqlite.accounts_expired ?? '—'}</span>
            <span class="mini-stat-label">过期</span>
          </div>
          <div class="mini-stat">
            <span class="mini-stat-val">{status.sqlite.accounts_temp ?? '—'}</span>
            <span class="mini-stat-label">临时</span>
          </div>
          <div class="mini-stat mini-stat-danger">
            <span class="mini-stat-val">{status.sqlite.accounts_failed ?? '—'}</span>
            <span class="mini-stat-label">失败</span>
          </div>
          <div class="mini-stat mini-stat-success">
            <span class="mini-stat-val">{status.sqlite.accounts_uploaded ?? '—'}</span>
            <span class="mini-stat-label">已上传</span>
          </div>
          <div class="mini-stat">
            <span class="mini-stat-val">{status.sqlite.accounts_not_uploaded ?? '—'}</span>
            <span class="mini-stat-label">未上传</span>
          </div>
        </div>
      {/if}
    </Panel>
  </div>

  <div class="dash-side">
    <Panel title="SQLite" subtitle={status?.sqlite.path ?? 'storage'}>
      {#if !status}
        <div style="display: grid; gap: 8px;">
          {#each Array(8) as _, i (i)}
            <div style="display: flex; justify-content: space-between;">
              <Skeleton width="50px" height="11px" />
              <Skeleton width="80px" height="11px" />
            </div>
          {/each}
        </div>
      {:else}
        <div class="kv-grid">
          <div class="kv-row"><span class="kv-key">数据库</span><span class="kv-val">{formatBytes(status.sqlite.size_bytes)}</span></div>
          <div class="kv-row"><span class="kv-key">WAL</span><span class="kv-val">{formatBytes(status.sqlite.wal_size_bytes)}</span></div>
          <div class="kv-row"><span class="kv-key">页面</span><span class="kv-val">{status.sqlite.page_count} × {formatBytes(status.sqlite.page_size)}</span></div>
          <div class="kv-row"><span class="kv-key">空闲页</span><span class="kv-val">{status.sqlite.freelist_count} ({formatPercent(status.sqlite.freelist_pct)})</span></div>
          <div class="kv-row"><span class="kv-key">Journal</span><span class="kv-val">{status.sqlite.journal_mode}</span></div>
          <div class="kv-row"><span class="kv-key">Sync</span><span class="kv-val">{sqliteSynchronousLabel(status.sqlite.synchronous)}</span></div>
          <div class="kv-row"><span class="kv-key">Checkpoint</span><span class="kv-val">{status.sqlite.wal_autocheckpoint} 页</span></div>
          <div class="kv-row"><span class="kv-key">Cache</span><span class="kv-val">{sqliteCacheSizeLabel(status.sqlite.cache_size)}</span></div>
          <div class="kv-row"><span class="kv-key">FK</span><span class="kv-val">{enabledLabel(status.sqlite.foreign_keys)}</span></div>
          <div class="kv-row"><span class="kv-key">Schema</span><span class="kv-val">v{status.sqlite.schema_version}</span></div>
          <div class="kv-row"><span class="kv-key">对象</span><span class="kv-val">{status.sqlite.tables_count}T {status.sqlite.indexes_count}I {status.sqlite.triggers_count}Tr</span></div>
          <div class="kv-row"><span class="kv-key">统计</span><span class="kv-val" style="font-size: 11px;">{formatUnixTime(status.sqlite.stats_updated_at)}</span></div>
        </div>
      {/if}
    </Panel>
  </div>
</section>
