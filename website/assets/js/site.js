// Zederia's website: the tour's tabs, copy buttons, the latest release and the repository's
// numbers from GitHub's API. Every page works without it: the HTML already holds release 1.0.1.
(() => {
  'use strict';
  const repo = 'LesToqz/zederia';
  const api = `https://api.github.com/repos/${repo}`;

  // --- The tour: tabs drawn like the instrument's page tabs ---------------------------------------
  document.querySelectorAll('[data-tabs]').forEach((root) => {
    const tabs = [...root.querySelectorAll('[role="tab"]')];
    const select = (tab, focus) => {
      tabs.forEach((t) => {
        const on = t === tab;
        t.setAttribute('aria-selected', String(on));
        t.tabIndex = on ? 0 : -1;
        document.getElementById(t.getAttribute('aria-controls')).hidden = !on;
      });
      if (focus) tab.focus();
    };
    tabs.forEach((tab, i) => {
      tab.addEventListener('click', () => select(tab, false));
      tab.addEventListener('keydown', (e) => {
        const step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
        if (step) { e.preventDefault(); select(tabs[(i + step + tabs.length) % tabs.length], true); }
        if (e.key === 'Home') { e.preventDefault(); select(tabs[0], true); }
        if (e.key === 'End') { e.preventDefault(); select(tabs[tabs.length - 1], true); }
      });
    });
  });

  // --- Copy buttons -----------------------------------------------------------------------------
  document.querySelectorAll('[data-copy]').forEach((button) => {
    const label = button.querySelector('span');
    const idle = label ? label.textContent : '';
    button.addEventListener('click', async () => {
      const source = document.getElementById(button.dataset.copy);
      if (!source) return;
      const text = source.dataset.copyText || source.textContent.trim();
      try {
        await navigator.clipboard.writeText(text);
        button.dataset.done = '';
        if (label) label.textContent = 'Copied';
        setTimeout(() => { delete button.dataset.done; if (label) label.textContent = idle; }, 1800);
      } catch {
        const range = document.createRange(); // no clipboard access: select it for the user
        range.selectNodeContents(source);
        const selection = getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
      }
    });
  });

  // --- The preview: still for people who ask for less motion -------------------------------------
  const preview = document.querySelector('video[data-preview]');
  if (preview && matchMedia('(prefers-reduced-motion: reduce)').matches) {
    preview.removeAttribute('autoplay');
    preview.pause();
    preview.controls = true;
  }

  const fill = (values) => {
    document.querySelectorAll('[data-release], [data-repo]').forEach((el) => {
      const key = el.dataset.release || el.dataset.repo;
      const value = values[key];
      if (value === undefined || value === null || value === '') return;
      if (el.dataset.attr) el.setAttribute(el.dataset.attr, value);
      else el.textContent = value;
    });
  };
  const getJson = (url) => fetch(url, { headers: { Accept: 'application/vnd.github+json' } })
    .then((r) => (r.ok ? r.json() : Promise.reject(new Error(String(r.status)))));

  // --- The latest release: version, date, file, size and checksum ---------------------------------
  if (document.querySelector('[data-release]')) {
    getJson(`${api}/releases/latest`).then((release) => {
      const asset = (release.assets || []).find((a) => /^ZederiaSetup-.+\.exe$/i.test(a.name));
      if (!asset) return;
      fill({
        version: release.tag_name.replace(/^v/, ''),
        date: new Date(release.published_at).toLocaleDateString('en-GB', { day: 'numeric', month: 'long', year: 'numeric' }),
        file: asset.name,
        url: asset.browser_download_url,
        size: `${Math.round(asset.size / 1048576)} MB`,
        sha256: (asset.digest || '').replace(/^sha256:/, ''),
        notes: release.html_url,
      });
    }).catch(() => {});
  }

  // --- The repository: when it last changed --------------------------------------------------------
  if (document.querySelector('[data-repo]')) {
    getJson(api).then((r) => {
      fill({ updated: new Date(r.pushed_at).toLocaleDateString('en-GB', { day: 'numeric', month: 'long', year: 'numeric' }) });
    }).catch(() => {});
  }
})();
