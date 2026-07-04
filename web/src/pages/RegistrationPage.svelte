<script lang="ts">
  import { onMount, tick } from 'svelte';
  import PageHeader from '../components/PageHeader.svelte';
  import Panel from '../components/Panel.svelte';
  import StatusBadge from '../components/StatusBadge.svelte';
  import EmptyState from '../components/EmptyState.svelte';
  import Modal from '../components/Modal.svelte';
  import Icon from '../components/Icon.svelte';
  import Skeleton from '../components/Skeleton.svelte';
  import { overflowDetect } from '../lib/actions';
  import { toast } from '../lib/toast';

  type RegistrationStatus = {
    ok: number; engine: string; provider_ready: number;
    active_tasks: number; active_flows: number; queued_flows: number;
    active_domains: number; active_proxies: number; temp_accounts: number;
  };

  type RegistrationTask = {
    id: string; status: string; mode?: string; workflow?: string;
    scheduler_mode?: string; register_provider?: string; target_metric?: string;
    count?: number; target_count?: number; concurrency: number;
    max_inflight?: number; oauth_delay_seconds?: number; detailed_logs: number;
    auto_upload_oauth_success?: number;
    infinite?: number; stop_requested?: number;
    started: number; active: number; success: number; failed: number;
    register_success?: number; register_failed?: number;
    oauth_success?: number; oauth_failed?: number; expired_written?: number;
    upload_success?: number; upload_failed?: number; upload_skipped?: number;
    fastlane_pre_email_active?: number; fastlane_waiting_email?: number;
    fastlane_post_email_active?: number; fastlane_alive_total?: number;
    created_ms: number; started_ms: number; updated_ms: number; finished_ms: number;
    error: string;
  };

  type RegistrationLog = {
    seq: number; ts_ms: number; level: string; flow_id: string; message: string;
  };

  type Workflow = 'register_only' | 'register_then_oauth';
  type SchedulerMode = 'normal' | 'fastlane';
  type TargetMetric = 'register_task' | 'oauth_success';
  type RegisterProvider = 'platform' | 'temporary';

  const emptyStatus: RegistrationStatus = {
    ok: 0, engine: 'curl-impersonate', provider_ready: 0,
    active_tasks: 0, active_flows: 0, queued_flows: 0,
    active_domains: 0, active_proxies: 0, temp_accounts: 0
  };

  const workflowOptions: { value: Workflow; label: string; hint: string }[] = [
    { value: 'register_only', label: '单独注册', hint: '只完成注册流程' },
    { value: 'register_then_oauth', label: '注册并 OAuth', hint: '注册成功后自动 OAuth' }
  ];

  const schedulerOptions: { value: SchedulerMode; label: string; hint: string }[] = [
    { value: 'normal', label: '常规', hint: '稳定的并发上限' },
    { value: 'fastlane', label: '高速', hint: '前后置错峰，吞吐更高' }
  ];

  const registerProviderOptions: { value: RegisterProvider; label: string; hint: string }[] = [
    { value: 'platform', label: '过期账号', hint: '复用过期账号继续注册' },
    { value: 'temporary', label: '临时账号', hint: '使用临时账号入口' }
  ];

  const targetOptions: { value: TargetMetric; label: string }[] = [
    { value: 'register_task', label: '注册任务数' },
    { value: 'oauth_success', label: 'OAuth 成功数' }
  ];

  let status = $state(emptyStatus);
  let workflow: Workflow = $state('register_only');
  let schedulerMode: SchedulerMode = $state('normal');
  let registerProvider: RegisterProvider = $state('platform');
  let targetMetric: TargetMetric = $state('register_task');
  let targetCount = $state(10);
  let concurrency = $state(20);
  let maxInflight = $state(20);
  let oauthDelaySeconds = $state(0);
  let infiniteMode = $state(false);
  let detailedLogs = $state(false);
  let autoUploadOauthSuccess = $state(false);
  let tasks: RegistrationTask[] = $state([]);
  let selectedTask: RegistrationTask | null = $state(null);
  let logs: RegistrationLog[] = $state([]);
  let starting = $state(false);
  let initialLoaded = $state(false);
  let wsState = $state<'connecting' | 'open' | 'closed' | 'error'>('closed');
  let ws: WebSocket | null = null;
  let logsOpen = $state(false);
  let detailLogList = $state(null as HTMLOListElement | null);
  let modalLogList = $state(null as HTMLOListElement | null);
  let expandedLogs: Set<number> = $state(new Set());
  let now = $state(Date.now());

  const LOG_STICK_THRESHOLD = 24;
  const MIN_VALID_EPOCH_MS = 1_600_000_000_000;

  let effectiveTargetMetric = $derived(workflow === 'register_only' ? 'register_task' : targetMetric);
  let selectedTaskCanStop = $derived(Boolean(selectedTask && ['queued', 'running', 'stopping'].includes(selectedTask.status)));
  let isFastlane = $derived(schedulerMode === 'fastlane');
  let isWithOauth = $derived(workflow === 'register_then_oauth');

  let statusCards = $derived([
    { label: '可用域名', value: status.active_domains, icon: 'tag' as const, klass: '' },
    { label: '活跃代理', value: status.active_proxies, icon: 'globe' as const, klass: '' },
    { label: '临时账号', value: status.temp_accounts, icon: 'inbox' as const, klass: '' },
    { label: '运行任务', value: status.active_tasks, icon: 'activity' as const, klass: status.active_tasks > 0 ? 'stat-chip-success' : '' },
    { label: '运行流程', value: status.active_flows, icon: 'zap' as const, klass: '' },
    { label: '等待流程', value: status.queued_flows, icon: 'clock' as const, klass: '' }
  ]);

  function statusLabel(value: string) {
    if (value === 'success') return '成功';
    if (value === 'partial') return '部分';
    if (value === 'failed') return '失败';
    if (value === 'running') return '运行中';
    if (value === 'queued') return '排队';
    if (value === 'stopping') return '停止中';
    if (value === 'stopped') return '已停止';
    return value || '-';
  }

  function statusVariant(value: string) {
    if (value === 'success') return 'active' as const;
    if (value === 'partial') return 'temp' as const;
    if (value === 'failed') return 'failed' as const;
    if (value === 'stopping' || value === 'stopped') return 'expired' as const;
    return 'default' as const;
  }

  function workflowLabel(value: string | undefined) {
    if (value === 'register_then_oauth') return '注册并 OAuth';
    if (value === 'oauth_only') return '仅 OAuth';
    return '单独注册';
  }

  function schedulerLabel(value: string | undefined) {
    return value === 'fastlane' ? '高速' : '常规';
  }

  function taskUsesFastlane(task: RegistrationTask | null) {
    return (task?.scheduler_mode ?? 'normal') === 'fastlane';
  }

  function metricLabel(value: string | undefined) {
    return value === 'oauth_success' ? 'OAuth 成功' : '注册任务';
  }

  function providerLabel(value: string | undefined) {
    return value === 'temporary' ? '临时账号' : '过期账号';
  }

  function taskProviderLabel(task: RegistrationTask | null) {
    if (!task) return '-';
    return (task.workflow ?? task.mode) === 'oauth_only' ? '账号池 OAuth' : providerLabel(task.register_provider);
  }

  function taskTargetLabel(task: RegistrationTask) {
    if (task.infinite) return '∞';
    return String(task.target_count ?? task.count ?? 0);
  }

  function progressPct(task: RegistrationTask) {
    if (task.infinite) return null;
    const target = task.target_count ?? task.count ?? 0;
    if (!target) return null;
    return Math.min(100, Math.max(0, (task.success / target) * 100));
  }

  function progressTone(task: RegistrationTask) {
    if (task.status === 'failed') return 'danger';
    if (task.status === 'stopping' || task.status === 'stopped') return 'warning';
    if (task.failed > 0 && task.success === 0) return 'danger';
    if (task.failed > 0 && task.failed > task.success) return 'warning';
    return 'success';
  }

  function formatRelativeTime(ms: number) {
    if (!isValidWallClockMs(ms)) return '时间待刷新';
    const diff = now - ms;
    if (diff < 5_000) return '刚刚';
    if (diff < 60_000) return `${Math.floor(diff / 1000)} 秒前`;
    if (diff < 3_600_000) return `${Math.floor(diff / 60_000)} 分钟前`;
    if (diff < 86_400_000) return `${Math.floor(diff / 3_600_000)} 小时前`;
    return new Date(ms).toLocaleString();
  }

  function isValidWallClockMs(ms: number) {
    return Number.isFinite(ms) && ms >= MIN_VALID_EPOCH_MS;
  }

  function formatAbsoluteTime(ms: number) {
    if (!isValidWallClockMs(ms)) return '时间待刷新';
    return new Date(ms).toLocaleString();
  }

  function formatDuration(start: number, end: number) {
    if (!isValidWallClockMs(start)) return '—';
    const endMs = isValidWallClockMs(end) ? end : now;
    const elapsed = endMs >= start ? endMs - start : 0;
    const total = Math.floor(elapsed / 1000);
    if (total < 60) return `${total}s`;
    const minutes = Math.floor(total / 60);
    const seconds = total % 60;
    if (minutes < 60) return `${minutes}分 ${seconds.toString().padStart(2, '0')}秒`;
    const hours = Math.floor(minutes / 60);
    const mins = minutes % 60;
    return `${hours}时 ${mins.toString().padStart(2, '0')}分`;
  }

  function taskElapsedMs(task: RegistrationTask | null) {
    if (!task || !isValidWallClockMs(task.started_ms)) return 0;
    const endMs = isValidWallClockMs(task.finished_ms) ? task.finished_ms : now;
    return endMs >= task.started_ms ? endMs - task.started_ms : 0;
  }

  function formatRatePerMinute(count: number | undefined, task: RegistrationTask | null) {
    const elapsed = taskElapsedMs(task);
    if (elapsed <= 0) return '0/分钟';
    const rate = (Number(count) || 0) * 60_000 / Math.max(elapsed, 1000);
    if (rate >= 100) return `${Math.round(rate)}/分钟`;
    if (rate >= 10) return `${rate.toFixed(1)}/分钟`;
    return `${rate.toFixed(2)}/分钟`;
  }

  function formatPercent(numerator: number | undefined, denominator: number | undefined) {
    const den = Number(denominator) || 0;
    if (den <= 0) return '—';
    const value = Math.max(0, Math.min(100, ((Number(numerator) || 0) / den) * 100));
    return `${value.toFixed(1)}%`;
  }

  function registrationSuccessRate(task: RegistrationTask | null) {
    if (!task) return '—';
    const success = task.register_success ?? 0;
    const failed = task.register_failed ?? 0;
    return formatPercent(success, success + failed);
  }

  function oauthSuccessRate(task: RegistrationTask | null) {
    if (!task) return '—';
    return formatPercent(task.oauth_success ?? 0, task.register_success ?? 0);
  }

  function trimFlowId(flowId: string) {
    if (!flowId) return 'task';
    return flowId.length > 14 ? `${flowId.slice(0, 14)}…` : flowId;
  }

  function formatLogTime(ts: number) {
    if (!ts) return '';
    const d = new Date(ts);
    const hh = d.getHours().toString().padStart(2, '0');
    const mm = d.getMinutes().toString().padStart(2, '0');
    const ss = d.getSeconds().toString().padStart(2, '0');
    return `${hh}:${mm}:${ss}`;
  }

  function isNearLogBottom(element: HTMLElement | null) {
    if (!element) return true;
    return element.scrollHeight - element.scrollTop - element.clientHeight <= LOG_STICK_THRESHOLD;
  }

  function scrollLogToBottom(element: HTMLElement | null) {
    if (!element) return;
    element.scrollTop = element.scrollHeight;
  }

  async function followLogTail(detailShouldStick = isNearLogBottom(detailLogList), modalShouldStick = isNearLogBottom(modalLogList)) {
    await tick();
    if (detailShouldStick) scrollLogToBottom(detailLogList);
    if (modalShouldStick) scrollLogToBottom(modalLogList);
  }

  async function openLogsModal() {
    logsOpen = true;
    await followLogTail(false, true);
  }

  function jumpDetailLogToBottom() {
    scrollLogToBottom(detailLogList);
  }

  function toggleLogExpanded(seq: number) {
    const next = new Set(expandedLogs);
    if (next.has(seq)) next.delete(seq);
    else next.add(seq);
    expandedLogs = next;
  }

  function handleLogKeydown(event: KeyboardEvent, seq: number) {
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      toggleLogExpanded(seq);
    }
  }

  function handleWorkflowChange() {
    if (workflow === 'register_only') targetMetric = 'register_task';
    if (workflow === 'register_only') autoUploadOauthSuccess = false;
  }

  async function loadStatus() {
    try {
      const response = await fetch('/api/registration/status');
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      status = await response.json();
    } catch (err) {
      console.warn('registration status load failed', err);
    }
  }

  async function loadTasks() {
    const response = await fetch('/api/registration/tasks');
    if (!response.ok) return;
    const data = await response.json();
    tasks = data.items ?? [];
    initialLoaded = true;
    if (!selectedTask && tasks.length > 0) {
      await selectTask(tasks[0].id);
    } else if (selectedTask) {
      const fresh = tasks.find((task) => task.id === selectedTask?.id);
      if (fresh) selectedTask = fresh;
      else {
        selectedTask = null;
        logs = [];
        logsOpen = false;
        if (tasks.length > 0) await selectTask(tasks[0].id);
      }
    }
  }

  async function selectTask(taskId: string) {
    const response = await fetch(`/api/registration/task?id=${encodeURIComponent(taskId)}`);
    const data = await response.json();
    if (data.ok && data.task) {
      selectedTask = data.task;
      logs = data.logs ?? [];
      subscribeTask(taskId);
      await followLogTail(true, true);
    } else if (selectedTask?.id === taskId) {
      selectedTask = null;
      logs = [];
      logsOpen = false;
    }
  }

  async function startTask(count: number, forceFinite = false) {
    starting = true;
    try {
      const safeCount = Math.max(1, Math.min(10000, Number(count) || 1));
      const safeConcurrency = Math.max(1, Math.min(5000, Number(concurrency) || 1));
      const safeMaxInflight = schedulerMode === 'fastlane'
        ? Math.max(1, Math.min(1000, Number(maxInflight) || 20))
        : safeConcurrency;
      const safeOauthDelay = workflow === 'register_then_oauth'
        ? Math.max(0, Math.min(3600, Number(oauthDelaySeconds) || 0))
        : 0;
      const useInfinite = !forceFinite && infiniteMode;
      const response = await fetch('/api/registration/start', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          workflow,
          scheduler_mode: schedulerMode,
          register_provider: registerProvider,
          target_metric: effectiveTargetMetric,
          target_count: useInfinite ? 0 : safeCount,
          concurrency: safeConcurrency,
          max_inflight: safeMaxInflight,
          oauth_delay_seconds: safeOauthDelay,
          infinite: useInfinite,
          auto_upload_oauth_success: isWithOauth && autoUploadOauthSuccess,
          detailed_logs: detailedLogs
        })
      });
      const data = await response.json();
      if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
      toast.success(`任务已创建：${data.task_id}`);
      await loadStatus();
      await loadTasks();
      await selectTask(data.task_id);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      starting = false;
    }
  }

  async function stopSelectedTask() {
    if (!selectedTask) return;
    starting = true;
    try {
      const response = await fetch('/api/registration/stop', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ task_id: selectedTask.id })
      });
      const data = await response.json();
      if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
      toast.success(`已请求停止：${selectedTask.id}`);
      await loadTasks();
      await selectTask(selectedTask.id);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      starting = false;
    }
  }

  function ensureWebSocket() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${protocol}//${window.location.host}/ws`);
    wsState = 'connecting';
    ws.onopen = () => { wsState = 'open'; if (selectedTask) subscribeTask(selectedTask.id); };
    ws.onclose = () => { wsState = 'closed'; };
    ws.onerror = () => { wsState = 'error'; };
    ws.onmessage = (event) => {
      try {
        const message = JSON.parse(event.data);
        if (message.type === 'registration_task' && message.payload?.task) {
          if (!selectedTask || message.payload.task.id === selectedTask.id) selectedTask = message.payload.task;
        } else if (message.type === 'registration_task' && message.payload?.ok === 0 && selectedTask) {
          selectedTask = null;
          logs = [];
          logsOpen = false;
        }
        if (message.type === 'registration_logs' && message.task_id === selectedTask?.id) {
          const nextLogs: RegistrationLog[] = message.logs ?? [];
          if (nextLogs.length > 0) {
            const detailShouldStick = isNearLogBottom(detailLogList);
            const modalShouldStick = isNearLogBottom(modalLogList);
            const seen = new Set(logs.map((item) => item.seq));
            logs = [...logs, ...nextLogs.filter((item) => !seen.has(item.seq))].slice(-1000);
            void followLogTail(detailShouldStick, modalShouldStick);
          }
        }
      } catch (err) { console.warn('registration ws parse failed', err); }
    };
  }

  function subscribeTask(taskId: string) {
    ensureWebSocket();
    if (ws?.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'registration_subscribe', task_id: taskId }));
    }
  }

  function wsLabel() {
    if (wsState === 'open') return '已连接';
    if (wsState === 'connecting') return '连接中';
    if (wsState === 'error') return '异常';
    return '断开';
  }

  function wsPillClass() {
    if (wsState === 'open') return 'pill pill-success';
    if (wsState === 'error') return 'pill pill-danger';
    if (wsState === 'connecting') return 'pill pill-warning';
    return 'pill';
  }

  onMount(() => {
    loadStatus(); loadTasks(); ensureWebSocket();
    const dataTimer = window.setInterval(() => { loadStatus(); loadTasks(); }, 2500);
    const clockTimer = window.setInterval(() => { now = Date.now(); }, 1000);
    return () => {
      window.clearInterval(dataTimer);
      window.clearInterval(clockTimer);
      ws?.close();
    };
  });
</script>

<PageHeader
  title="注册工作台"
  subtitle="注册"
  description="按目标数量批量注册账号；可选高速调度，注册后立即执行 OAuth。"
>
  <span class={wsPillClass()} title="WebSocket 状态" style="margin-right: 4px;">
    <Icon name={wsState === 'open' ? 'wifi' : 'wifi-off'} size={11} />
    {wsLabel()}
  </span>
  <button class="btn" type="button" onclick={loadStatus}>
    <Icon name="refresh" size={14} />
    刷新
  </button>
  <button class="btn btn-danger" type="button" onclick={stopSelectedTask} disabled={starting || !selectedTaskCanStop}>
    <Icon name="stop" size={12} />
    停止当前
  </button>
</PageHeader>

<section class="stat-strip" aria-label="注册状态">
  {#each statusCards as item}
    <div class={`stat-chip ${item.klass}`}>
      <span class="stat-chip-label"><Icon name={item.icon} size={12} /> {item.label}</span>
      <span class="stat-chip-value">{item.value}</span>
    </div>
  {/each}
</section>

<Panel title="任务编排" subtitle="流程">
  {#snippet badge()}
    <span class="pill pill-info">{providerLabel(registerProvider)}</span>
    <span class="pill">{workflowLabel(workflow)}</span>
    <span class={isFastlane ? 'pill pill-success' : 'pill'}>
      {#if isFastlane}<Icon name="zap" size={11} />{/if}
      {schedulerLabel(schedulerMode)}
    </span>
  {/snippet}

  <div class="orch">
    <div class="orch-section">
      <span class="orch-section-title">流程编排</span>
      <div class="orch-row orch-row-2">
        <div class="orch-field">
          <span class="orch-label">账号来源</span>
          <div class="segmented segmented-block">
            {#each registerProviderOptions as opt}
              <button type="button" class:active={registerProvider === opt.value} onclick={() => registerProvider = opt.value} disabled={starting}>{opt.label}</button>
            {/each}
          </div>
          <span class="orch-hint">{registerProviderOptions.find((o) => o.value === registerProvider)?.hint}</span>
        </div>
        <div class="orch-field">
          <span class="orch-label">编排方式</span>
          <div class="segmented segmented-block">
            {#each workflowOptions as opt}
              <button type="button" class:active={workflow === opt.value} onclick={() => { workflow = opt.value; handleWorkflowChange(); }} disabled={starting}>{opt.label}</button>
            {/each}
          </div>
          <span class="orch-hint">{workflowOptions.find((o) => o.value === workflow)?.hint}</span>
        </div>
      </div>
    </div>

    <div class="orch-section">
      <span class="orch-section-title">目标 & 调度</span>
      <div class="orch-row">
        <div class="orch-field orch-field-num">
          <span class="orch-label">目标数量</span>
          <input class="input" type="number" min="1" max="10000" bind:value={targetCount} disabled={starting || infiniteMode} />
          <span class="orch-hint">{infiniteMode ? '已启用无限模式' : '单次任务的目标数'}</span>
        </div>
        {#if isWithOauth}
          <div class="orch-field">
            <span class="orch-label">目标口径</span>
            <select class="input" bind:value={targetMetric} disabled={starting}>
              {#each targetOptions as opt}<option value={opt.value}>{opt.label}</option>{/each}
            </select>
            <span class="orch-hint">达成该口径数量后任务结束</span>
          </div>
        {/if}
        <div class="orch-field">
          <span class="orch-label">调度模式</span>
          <div class="segmented segmented-block">
            {#each schedulerOptions as opt}
              <button type="button" class:active={schedulerMode === opt.value} onclick={() => schedulerMode = opt.value} disabled={starting}>{opt.label}</button>
            {/each}
          </div>
          <span class="orch-hint">{schedulerOptions.find((o) => o.value === schedulerMode)?.hint}</span>
        </div>
      </div>

      <div class="orch-row">
        <div class="orch-field orch-field-num">
          <span class="orch-label">{isFastlane ? '前置并发' : '并发上限'}</span>
          <input class="input" type="number" min="1" max="5000" bind:value={concurrency} disabled={starting} />
          <span class="orch-hint">{isFastlane ? '注册前阶段同时进行的流程数' : '同时进行的注册流程数'}</span>
        </div>
        {#if isFastlane}
          <div class="orch-field orch-field-num">
            <span class="orch-label">最大存活</span>
            <input class="input" type="number" min="1" max="1000" bind:value={maxInflight} disabled={starting} />
            <span class="orch-hint">前后置加起来的总上限</span>
          </div>
        {/if}
        {#if isWithOauth}
          <div class="orch-field orch-field-num">
            <span class="orch-label">OAuth 延迟</span>
            <div class="input-suffix">
              <input class="input" type="number" min="0" max="3600" step="1" bind:value={oauthDelaySeconds} disabled={starting} />
              <span class="input-suffix-text">秒</span>
            </div>
            <span class="orch-hint">注册成功后等待秒数</span>
          </div>
        {/if}
      </div>

      <div class="orch-options">
        <label class="toggle-row">
          <input type="checkbox" bind:checked={infiniteMode} disabled={starting} />
          <span>无限模式</span>
        </label>
        <label class="toggle-row">
          <input type="checkbox" bind:checked={detailedLogs} disabled={starting} />
          <span>详细日志</span>
        </label>
        {#if isWithOauth}
          <label class="toggle-row">
            <input type="checkbox" bind:checked={autoUploadOauthSuccess} disabled={starting} />
            <span>OAuth 成功后自动上传</span>
          </label>
        {/if}
      </div>
    </div>

    <div class="orch-actions">
      <button class="btn" type="button" onclick={() => startTask(1, true)} disabled={starting}>
        <Icon name="play" size={12} />
        单次执行
      </button>
      <button class="btn btn-primary" type="button" onclick={() => startTask(targetCount)} disabled={starting} data-loading={starting}>
        <Icon name={infiniteMode ? 'activity' : 'zap'} size={12} />
        {infiniteMode ? '启动无限任务' : `启动 ${targetCount} 个目标`}
      </button>
      <span class="flex-spacer"></span>
      <button class="btn btn-danger" type="button" onclick={stopSelectedTask} disabled={starting || !selectedTaskCanStop}>
        <Icon name="stop" size={12} />
        停止选中任务
      </button>
    </div>
  </div>
</Panel>

<section class="two-col-grid two-col-wide">
  <Panel title="任务列表" subtitle={`${tasks.length} 个`}>
    {#if !initialLoaded}
      <div style="display: grid; gap: 8px;">
        {#each Array(3) as _, i (i)}
          <Skeleton height="92px" rounded="md" />
        {/each}
      </div>
    {:else if tasks.length === 0}
      <EmptyState
        title="暂无任务"
        message="使用上方的「单次执行」或「启动目标任务」开启第一个流程"
        icon="zap"
      />
    {:else}
      <div class="task-list">
        {#each tasks as task (task.id)}
          {@const pct = progressPct(task)}
          {@const tone = progressTone(task)}
          {@const fastlane = taskUsesFastlane(task)}
          <button
            type="button"
            class="task-card"
            class:task-card-active={selectedTask?.id === task.id}
            onclick={() => selectTask(task.id)}
          >
            <div class="task-card-info">
              <div class="task-card-head">
                <span class="task-card-id text-mono">{task.id}</span>
                <StatusBadge label={statusLabel(task.status)} variant={statusVariant(task.status)} />
              </div>
              <div class="task-card-tags">
                <span class="tag">{taskProviderLabel(task)}</span>
                <span class="tag">{workflowLabel(task.workflow ?? task.mode)}</span>
                {#if fastlane}<span class="tag tag-success">高速</span>{/if}
                {#if task.auto_upload_oauth_success}<span class="tag tag-info">自动上传</span>{/if}
                <span class="text-muted">{formatRelativeTime(task.created_ms)}</span>
              </div>

              {#if pct !== null || task.infinite}
                <div class="task-card-progress-row">
                  <span class="task-card-progress-text">
                    {task.success}<span class="text-muted" style="font-weight: 400;">/{taskTargetLabel(task)}</span>
                  </span>
                  <div class="task-card-progress" style="flex: 1;">
                    <span
                      class:is-infinite={task.infinite}
                      style={`width: ${task.infinite ? 100 : pct}%; background: ${tone === 'danger' ? 'var(--color-danger)' : tone === 'warning' ? '#d4a017' : 'var(--color-success)'};`}
                    ></span>
                  </div>
                  {#if pct !== null}
                    <span class="task-card-progress-pct">{Math.round(pct)}%</span>
                  {/if}
                </div>
              {/if}

              <div class="task-card-stats">
                <span class="task-stat-pip">
                  <span class="dot dot-success"></span>
                  <span class="text-muted">注册</span>
                  <strong>{task.register_success ?? 0}</strong>
                </span>
                {#if (task.workflow ?? task.mode) !== 'register_only' && (task.oauth_success ?? 0) > 0}
                  <span class="task-stat-pip">
                    <span class="dot dot-info"></span>
                    <span class="text-muted">OAuth</span>
                    <strong>{task.oauth_success}</strong>
                  </span>
                {/if}
                {#if task.failed > 0}
                  <span class="task-stat-pip">
                    <span class="dot dot-danger"></span>
                    <span class="text-muted">失败</span>
                    <strong style="color: var(--color-danger);">{task.failed}</strong>
                  </span>
                {/if}
                {#if task.active > 0}
                  <span class="task-stat-pip">
                    <span class="spinner" style="width: 8px; height: 8px; color: var(--color-text-muted);"></span>
                    <span class="text-muted">运行</span>
                    <strong>{task.active}</strong>
                  </span>
                {/if}
                {#if task.auto_upload_oauth_success && ((task.upload_success ?? 0) > 0 || (task.upload_failed ?? 0) > 0 || (task.upload_skipped ?? 0) > 0)}
                  <span class="task-stat-pip task-stat-pip-end">
                    <span class="text-muted">上传</span>
                    <strong>{task.upload_success ?? 0}</strong>
                    {#if (task.upload_failed ?? 0) > 0}
                      <span style="color: var(--color-danger);">/{task.upload_failed}</span>
                    {/if}
                  </span>
                {/if}
              </div>
            </div>
          </button>
        {/each}
      </div>
    {/if}
  </Panel>

  <Panel title="任务详情" subtitle={selectedTask ? selectedTask.id : '实时'}>
    {#snippet actions()}
      {#if selectedTask}
        <button class="btn btn-sm" type="button" onclick={openLogsModal}>
          <Icon name="list" size={12} />
          全部日志 ({logs.length})
        </button>
      {/if}
    {/snippet}

    {#if !selectedTask}
      <EmptyState
        title="未选中任务"
        message="从左侧任务列表选择一项以查看实时状态"
        icon="info"
      />
    {:else}
      {@const fastlane = taskUsesFastlane(selectedTask)}
      {@const isRunning = ['queued', 'running', 'stopping'].includes(selectedTask.status)}
      {@const pct = progressPct(selectedTask)}
      {@const tone = progressTone(selectedTask)}

      <div class="task-detail-head">
        <div class="task-detail-tags">
          <StatusBadge label={statusLabel(selectedTask.status)} variant={statusVariant(selectedTask.status)} />
          <span class="tag">{taskProviderLabel(selectedTask)}</span>
          <span class="tag">{workflowLabel(selectedTask.workflow ?? selectedTask.mode)}</span>
          {#if fastlane}
            <span class="tag tag-success">高速</span>
          {/if}
          {#if selectedTask.auto_upload_oauth_success}
            <span class="tag tag-info">自动上传</span>
          {/if}
          <span class="tag">{metricLabel(selectedTask.target_metric)}</span>
        </div>
        <div class="task-detail-meta">
          <span class="task-detail-meta-item">
            <Icon name="clock" size={11} />
            创建 {formatAbsoluteTime(selectedTask.created_ms)}
          </span>
          {#if selectedTask.started_ms}
            <span class="task-detail-meta-item">
              <Icon name="play" size={11} />
              运行 {formatDuration(selectedTask.started_ms, selectedTask.finished_ms)}
            </span>
            <span class="task-detail-meta-item">
              <Icon name="activity" size={11} />
              注册速度 {formatRatePerMinute(selectedTask.register_success, selectedTask)}
            </span>
            {#if (selectedTask.workflow ?? selectedTask.mode) !== 'register_only'}
              <span class="task-detail-meta-item">
                <Icon name="zap" size={11} />
                OAuth 速度 {formatRatePerMinute(selectedTask.oauth_success, selectedTask)}
              </span>
            {/if}
          {/if}
          {#if selectedTask.finished_ms}
            <span class="task-detail-meta-item">
              <Icon name="check" size={11} />
              结束 {formatRelativeTime(selectedTask.finished_ms)}
            </span>
          {/if}
        </div>
      </div>

      {#if pct !== null || selectedTask.infinite}
        <div class="task-detail-progress">
          <div class="task-detail-progress-head">
            <span class="task-detail-progress-label">进度</span>
            <span class="task-detail-progress-value">
              {selectedTask.success}<span class="text-muted"> / {taskTargetLabel(selectedTask)}</span>
              {#if pct !== null}<span class="task-detail-progress-value-pct">{pct.toFixed(1)}%</span>{/if}
            </span>
          </div>
          <div class="gauge-bar">
            <span class="gauge-fill"
              class:gauge-fill-warning={tone === 'warning'}
              class:gauge-fill-danger={tone === 'danger'}
              style={`width: ${selectedTask.infinite ? 100 : pct}%; ${selectedTask.infinite ? 'opacity: 0.5;' : ''}`}></span>
          </div>
        </div>
      {/if}

      <div class="task-stats">
        <div class="task-stat">
          <span class="task-stat-label">运行中</span>
          <span class="task-stat-value">{selectedTask.active}</span>
        </div>
        <div class="task-stat">
          <span class="task-stat-label">注册成功</span>
          <span class="task-stat-value" style="color: var(--color-success);">{selectedTask.register_success ?? 0}</span>
        </div>
        <div class="task-stat">
          <span class="task-stat-label">注册成功率</span>
          <span class="task-stat-value" style="color: var(--color-success); font-size: 14px;">{registrationSuccessRate(selectedTask)}</span>
        </div>
        {#if (selectedTask.workflow ?? selectedTask.mode) !== 'register_only'}
          <div class="task-stat">
            <span class="task-stat-label">OAuth 成功</span>
            <span class="task-stat-value" style="color: var(--color-success);">{selectedTask.oauth_success ?? 0}</span>
          </div>
          <div class="task-stat">
            <span class="task-stat-label">OAuth 成功率</span>
            <span class="task-stat-value" style="color: var(--color-success); font-size: 14px;">{oauthSuccessRate(selectedTask)}</span>
          </div>
        {/if}
        {#if selectedTask.auto_upload_oauth_success}
          <div class="task-stat">
            <span class="task-stat-label">上传成功</span>
            <span class="task-stat-value" style="color: var(--color-success);">{selectedTask.upload_success ?? 0}</span>
          </div>
          <div class="task-stat">
            <span class="task-stat-label">上传失败 / 跳过</span>
            <span class="task-stat-value" style={(selectedTask.upload_failed ?? 0) > 0 ? 'color: var(--color-danger);' : 'font-size: 14px;'}>
              {selectedTask.upload_failed ?? 0}<span class="text-muted" style="font-weight: 400;"> / {selectedTask.upload_skipped ?? 0}</span>
            </span>
          </div>
        {/if}
        <div class="task-stat">
          <span class="task-stat-label">失败</span>
          <span class="task-stat-value" style={selectedTask.failed > 0 ? 'color: var(--color-danger);' : ''}>{selectedTask.failed}</span>
        </div>
      </div>

      {#if fastlane}
        <div class="fastlane-card">
          <div class="fastlane-card-title">
            <Icon name="zap" size={11} />
            高速模式 · 实时阶段
          </div>
          <div class="fastlane-grid">
            <div>
              <div class="fastlane-cell-label">前置</div>
              <div class="fastlane-cell-value">{selectedTask.fastlane_pre_email_active ?? 0}</div>
            </div>
            <div>
              <div class="fastlane-cell-label">等邮箱</div>
              <div class="fastlane-cell-value">{selectedTask.fastlane_waiting_email ?? 0}</div>
            </div>
            <div>
              <div class="fastlane-cell-label">后置</div>
              <div class="fastlane-cell-value">{selectedTask.fastlane_post_email_active ?? 0}</div>
            </div>
            <div>
              <div class="fastlane-cell-label">存活 / 上限</div>
              <div class="fastlane-cell-value">
                {selectedTask.fastlane_alive_total ?? selectedTask.active}<span class="text-muted">/{selectedTask.max_inflight ?? selectedTask.concurrency}</span>
              </div>
            </div>
          </div>
        </div>
      {/if}

      {#if selectedTask.error}
        <div class="notice-bar notice-error" style="margin: 14px 0 0;">
          <span class="notice-icon"><Icon name="alert-circle" size={14} /></span>
          <span class="notice-text">{selectedTask.error}</span>
        </div>
      {/if}

      <div class="task-detail-section-title">
        {#if isRunning}<span class="spinner" style="width: 10px; height: 10px; border-width: 1.4px;"></span>{/if}
        实时日志
        <span class="spacer"></span>
        <span class="meta">
          显示最近 {Math.min(logs.length, 80)} / 共 {logs.length}
          {#if logs.length > 80}
            <button class="btn-link" type="button" style="margin-left: 8px; font-size: 11px;" onclick={openLogsModal}>看全部</button>
          {/if}
        </span>
      </div>

      {#if logs.length === 0}
        <div class="empty-card" style="padding: 24px 16px;">
          <Icon name="info" size={14} class="text-muted" />
          <span style="margin-left: 6px;">暂无日志输出</span>
        </div>
      {:else}
        <div style="position: relative;">
          <ol class="log-list log-list-scroll" bind:this={detailLogList}>
            {#each logs.slice(-80) as item (item.seq)}
              <li
                class="log-item"
                class:log-error={item.level === 'error'}
                class:log-warn={item.level === 'warn'}
                class:log-debug={item.level === 'debug'}
                class:is-expanded={expandedLogs.has(item.seq)}
                role="button"
                tabindex="0"
                onclick={() => toggleLogExpanded(item.seq)}
                onkeydown={(event) => handleLogKeydown(event, item.seq)}
              >
                <span class="log-flow text-mono">{formatLogTime(item.ts_ms)} · {trimFlowId(item.flow_id)}</span>
                <span class="log-msg" use:overflowDetect>{item.message}</span>
                <span class="log-chevron" aria-hidden="true">
                  <Icon name="chevron-down" size={12} />
                </span>
              </li>
            {/each}
          </ol>
          {#if isRunning}
            <button
              type="button"
              class="btn btn-xs log-jump-btn"
              onclick={jumpDetailLogToBottom}
              title="跳到最新"
            >
              <Icon name="chevron-down" size={11} />
              最新
            </button>
          {/if}
        </div>
      {/if}
    {/if}
  </Panel>
</section>

<Modal
  open={logsOpen}
  title="任务日志"
  kicker={selectedTask ? selectedTask.id : 'LOGS'}
  subtitle={selectedTask ? `${statusLabel(selectedTask.status)} · 共 ${logs.length} 条` : `共 ${logs.length} 条`}
  size="lg"
  onclose={() => logsOpen = false}
>
  {#if logs.length === 0}
    <EmptyState message="暂无日志输出" icon="info" />
  {:else}
    <ol class="log-list" style="max-height: none;" bind:this={modalLogList}>
      {#each logs as item (item.seq)}
        <li
          class="log-item"
          class:log-error={item.level === 'error'}
          class:log-warn={item.level === 'warn'}
          class:log-debug={item.level === 'debug'}
          class:is-expanded={expandedLogs.has(item.seq)}
          role="button"
          tabindex="0"
          onclick={() => toggleLogExpanded(item.seq)}
          onkeydown={(event) => handleLogKeydown(event, item.seq)}
        >
          <span class="log-flow text-mono">{formatLogTime(item.ts_ms)} · {trimFlowId(item.flow_id)}</span>
          <span class="log-msg" use:overflowDetect>{item.message}</span>
          <span class="log-chevron" aria-hidden="true">
            <Icon name="chevron-down" size={12} />
          </span>
        </li>
      {/each}
    </ol>
  {/if}

  {#snippet footer()}
    <button class="btn btn-sm" type="button" onclick={() => scrollLogToBottom(modalLogList)}>
      <Icon name="chevron-down" size={11} />
      跳到最新
    </button>
    <span class="flex-spacer"></span>
    <button class="btn btn-sm" type="button" onclick={() => logsOpen = false}>关闭</button>
  {/snippet}
</Modal>
