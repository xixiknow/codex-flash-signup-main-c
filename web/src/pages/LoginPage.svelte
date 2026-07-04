<script lang="ts">
  import Icon from '../components/Icon.svelte';
  import { toast } from '../lib/toast';

  interface Props {
    onLoggedIn: (username: string) => void;
  }

  let { onLoggedIn }: Props = $props();
  let username = $state('admin');
  let password = $state('');
  let busy = $state(false);
  let showPassword = $state(false);

  async function submitLogin(event: SubmitEvent) {
    event.preventDefault();
    busy = true;
    try {
      const response = await fetch('/api/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          username: username.trim(),
          password
        })
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || !data.ok) {
        throw new Error(data.error || `HTTP ${response.status}`);
      }
      const next = data.user?.username ?? username.trim();
      toast.success(`欢迎回来，${next}`);
      onLoggedIn(next);
    } catch (err) {
      toast.error(err instanceof Error ? err.message : String(err));
    } finally {
      busy = false;
    }
  }
</script>

<main class="login-shell">
  <section class="login-panel" aria-labelledby="login-title">
    <div class="login-brand">
      <span class="login-mark">M</span>
      <div>
        <p class="login-kicker">Mongoose 控制台</p>
        <h1 id="login-title">欢迎回来</h1>
      </div>
    </div>

    <form class="login-form" onsubmit={submitLogin}>
      <label class="field">
        <span>用户名</span>
        <div style="position: relative; display: flex; align-items: center;">
          <span style="position: absolute; left: 12px; color: var(--color-text-muted); pointer-events: none; display: inline-flex;">
            <Icon name="user" size={14} />
          </span>
          <input
            class="input"
            style="padding-left: 36px;"
            bind:value={username}
            autocomplete="username"
            required
            disabled={busy}
            placeholder="账号"
          />
        </div>
      </label>
      <label class="field">
        <span>密码</span>
        <div style="position: relative; display: flex; align-items: center;">
          <span style="position: absolute; left: 12px; color: var(--color-text-muted); pointer-events: none; display: inline-flex;">
            <Icon name="lock" size={14} />
          </span>
          <input
            class="input"
            style="padding-left: 36px; padding-right: 40px;"
            bind:value={password}
            type={showPassword ? 'text' : 'password'}
            autocomplete="current-password"
            required
            disabled={busy}
            placeholder="密码"
          />
          <button
            type="button"
            class="search-input-clear"
            style="right: 8px;"
            onclick={() => showPassword = !showPassword}
            disabled={busy}
            aria-label={showPassword ? '隐藏密码' : '显示密码'}
          >
            <Icon name={showPassword ? 'eye-off' : 'eye'} size={14} />
          </button>
        </div>
      </label>
      <button class="btn btn-primary btn-lg login-submit" type="submit" disabled={busy || username.trim() === '' || password === ''} data-loading={busy}>
        {busy ? '正在登录…' : '登录控制台'}
      </button>
    </form>

    <div style="display: flex; align-items: center; gap: 8px; margin-top: 22px; padding-top: 18px; border-top: 1px solid var(--color-border); color: var(--color-text-muted); font-size: 12px;">
      <Icon name="shield" size={12} />
      <span>会话仅在本机有效，关闭浏览器后失效</span>
    </div>
  </section>
</main>
