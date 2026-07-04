<script lang="ts">
  import { onMount } from 'svelte';
  import ConsolePage from './pages/ConsolePage.svelte';
  import AccountPage from './pages/AccountPage.svelte';
  import LoginPage from './pages/LoginPage.svelte';
  import MailConfigPage from './pages/MailConfigPage.svelte';
  import RedeemPage from './pages/RedeemPage.svelte';
  import ProxyPoolPage from './pages/ProxyPoolPage.svelte';
  import RegistrationPage from './pages/RegistrationPage.svelte';
  import UploadConfigPage from './pages/UploadConfigPage.svelte';
  import ToastRegion from './components/ToastRegion.svelte';
  import Icon, { type IconName } from './components/Icon.svelte';
  import { pathForRoute, routeFromPath, type AppRoute } from './lib/routes';

  const routeTitles: Record<AppRoute, string> = {
    login: '登录',
    console: '控制台',
    'proxy-pool': '代理池',
    mail: '邮件',
    redeem: '兑换码',
    accounts: '账号管理',
    registration: '注册工作台',
    'upload-config': '上传配置'
  };

  const navItems: { route: AppRoute; label: string; icon: IconName }[] = [
    { route: 'console', label: '控制台', icon: 'dashboard' },
    { route: 'proxy-pool', label: '代理池', icon: 'globe' },
    { route: 'mail', label: '邮件', icon: 'mail' },
    { route: 'redeem', label: '兑换码', icon: 'tag' },
    { route: 'accounts', label: '账号管理', icon: 'user' },
    { route: 'registration', label: '注册工作台', icon: 'sparkles' },
    { route: 'upload-config', label: '上传配置', icon: 'upload' }
  ];

  let route: AppRoute = $state(routeFromPath(window.location.pathname));
  let authChecked = $state(false);
  let authenticated = $state(false);
  let username = $state('');
  let authDisabled = $state(false);
  let drawerOpen = $state(false);

  let userInitial = $derived((authDisabled ? 'L' : (username || '?').charAt(0)).toUpperCase());

  function setRoute(next: AppRoute, replace = false) {
    const path = pathForRoute(next);
    route = next;
    document.title = `Mongoose · ${routeTitles[next]}`;
    if (window.location.pathname !== path) {
      const method = replace ? window.history.replaceState : window.history.pushState;
      method.call(window.history, {}, '', path);
    }
  }

  function syncRouteFromLocation() {
    setRoute(routeFromPath(window.location.pathname), true);
  }

  function navigate(event: MouseEvent, next: AppRoute) {
    event.preventDefault();
    drawerOpen = false;
    setRoute(next);
  }

  async function checkAuth() {
    try {
      const response = await fetch('/api/auth/me');
      const data = await response.json();
      authenticated = Boolean(data.authenticated);
      authDisabled = Boolean(data.auth_disabled);
      username = data.user?.username ?? '';
      authChecked = true;
      if (!authenticated && route !== 'login') {
        setRoute('login', true);
      } else if (authenticated && route === 'login') {
        setRoute('console', true);
      }
    } catch (err) {
      console.error('鉴权状态检查失败', err);
      authenticated = false;
      authChecked = true;
      setRoute('login', true);
    }
  }

  function handleLoggedIn(nextUsername: string) {
    authenticated = true;
    username = nextUsername;
    authChecked = true;
    setRoute('console', true);
  }

  async function logout() {
    try {
      await fetch('/api/auth/logout', { method: 'POST' });
    } finally {
      authenticated = false;
      username = '';
      drawerOpen = false;
      setRoute('login');
    }
  }

  function toggleDrawer() {
    drawerOpen = !drawerOpen;
  }

  function closeDrawer() {
    drawerOpen = false;
  }

  function handleDrawerKey(event: KeyboardEvent) {
    if (event.key === 'Escape') closeDrawer();
  }

  onMount(() => {
    syncRouteFromLocation();
    checkAuth();
    window.addEventListener('popstate', syncRouteFromLocation);
    return () => window.removeEventListener('popstate', syncRouteFromLocation);
  });
</script>

<ToastRegion />

{#if !authChecked}
  <main class="boot-shell">
    <section class="boot-card" style="text-align: center;">
      <span class="topbar-brand-mark" style="width: 56px; height: 56px; font-size: 24px; border-radius: 14px; margin: 0 auto 18px;">M</span>
      <h1>Mongoose 控制台</h1>
      <p>正在检查登录状态…</p>
    </section>
  </main>
{:else if !authenticated || route === 'login'}
  <LoginPage onLoggedIn={handleLoggedIn} />
{:else}
<div class="shell">
  <aside class="sidebar" aria-label="主导航">
    <p class="sidebar-brand">
      <span class="topbar-brand-mark">M</span>
      Mongoose
    </p>
    <nav class="site-nav" aria-label="栏目">
      {#each navItems as item}
        <a
          href={pathForRoute(item.route)}
          class:active={route === item.route}
          aria-current={route === item.route ? 'page' : undefined}
          onclick={(event) => navigate(event, item.route)}
        >
          <Icon name={item.icon} size={16} strokeWidth={1.6} />
          {item.label}
        </a>
      {/each}
    </nav>
    <div class="sidebar-footer">
      <div class="sidebar-user">
        <span class="sidebar-user-avatar">{userInitial}</span>
        <div class="sidebar-user-info">
          <span class="sidebar-user-label">{authDisabled ? '本地模式' : '已登录'}</span>
          <strong>{authDisabled ? '调试用户' : username}</strong>
        </div>
      </div>
      <button class="btn btn-ghost btn-sm" type="button" onclick={logout} style="justify-content: flex-start; gap: 8px;">
        <Icon name="logout" size={14} />
        退出登录
      </button>
    </div>
  </aside>

  <div class="page-shell">
    <header class="topbar">
      <div class="topbar-inner">
        <button class="topbar-toggle" type="button" onclick={toggleDrawer} aria-label="打开导航">
          <Icon name="menu" size={18} strokeWidth={1.7} />
        </button>
        <p class="topbar-brand">
          <span class="topbar-brand-mark">M</span>
          Mongoose
        </p>
        <span class="topbar-route" aria-hidden="true">{routeTitles[route]}</span>
        <span class="topbar-user-chip" title={username}>
          <span style="display: inline-grid; place-items: center; width: 18px; height: 18px; border-radius: 50%; background: var(--color-accent); color: #fff; font-size: 10px; font-weight: 700;">{userInitial}</span>
          {authDisabled ? '本地' : (username || '游客')}
        </span>
      </div>
    </header>

    {#if drawerOpen}
      <div
        class="nav-drawer-backdrop is-open"
        role="presentation"
        onclick={closeDrawer}
        onkeydown={handleDrawerKey}
      ></div>
      <aside class="nav-drawer" aria-label="移动导航">
        <div class="nav-drawer-head">
          <h2 class="nav-drawer-title">
            <span class="nav-drawer-title-mark">M</span>
            Mongoose
          </h2>
          <button class="nav-drawer-close" type="button" onclick={closeDrawer} aria-label="关闭导航">×</button>
        </div>
        <nav class="site-nav" aria-label="栏目">
          {#each navItems as item}
            <a
              href={pathForRoute(item.route)}
              class:active={route === item.route}
              aria-current={route === item.route ? 'page' : undefined}
              onclick={(event) => navigate(event, item.route)}
            >
              <Icon name={item.icon} size={16} strokeWidth={1.6} />
              {item.label}
            </a>
          {/each}
        </nav>
        <div class="sidebar-footer">
          <div class="sidebar-user">
            <span class="sidebar-user-avatar">{userInitial}</span>
            <div class="sidebar-user-info">
              <span class="sidebar-user-label">{authDisabled ? '本地模式' : '已登录'}</span>
              <strong>{authDisabled ? '调试用户' : username}</strong>
            </div>
          </div>
          <button class="btn btn-ghost btn-sm" type="button" onclick={logout} style="justify-content: flex-start; gap: 8px;">
            <Icon name="logout" size={14} />
            退出登录
          </button>
        </div>
      </aside>
    {/if}

    <main class="page-content">
      {#if route === 'console'}
        <ConsolePage />
      {:else if route === 'proxy-pool'}
        <ProxyPoolPage />
      {:else if route === 'mail'}
        <MailConfigPage />
      {:else if route === 'redeem'}
        <RedeemPage />
      {:else if route === 'accounts'}
        <AccountPage />
      {:else if route === 'registration'}
        <RegistrationPage />
      {:else if route === 'upload-config'}
        <UploadConfigPage />
      {:else}
        <ConsolePage />
      {/if}
    </main>
  </div>
</div>
{/if}
