// Compatibility table loaded from compat_data.json.
(function () {
  const PAGE_SIZE = 50;
  let allGames = [];
  let filtered = [];
  let page = 0;
  let activeFilter = 'all';
  let searchQuery = '';

  function formatNumber(n) {
    return n.toLocaleString('en-US');
  }

  function statusBadge(status, label) {
    if (status === 'ok') return `<span class="badge badge-ok">Working</span>`;
    if (status === 'blank') return `<span class="badge badge-blank">Blank Screen</span>`;
    return `<span class="badge badge-review">Needs Review</span>`;
  }

  function platformBadge(platform) {
    const cls = platform.startsWith('GBC') ? 'badge-gbc' : 'badge-dmg';
    return `<span class="badge ${cls}">${platform}</span>`;
  }

  function renderTable() {
    const tbody = document.getElementById('compat-tbody');
    const empty = document.getElementById('compat-empty');
    const start = page * PAGE_SIZE;
    const slice = filtered.slice(start, start + PAGE_SIZE);

    if (filtered.length === 0) {
      tbody.innerHTML = '';
      empty.classList.remove('hidden');
    } else {
      empty.classList.add('hidden');
      tbody.innerHTML = slice.map(g => `
        <tr>
          <td>${g.title}</td>
          <td>${platformBadge(g.platform)}</td>
          <td style="color:var(--muted);font-size:0.8rem">${g.mbc}</td>
          <td>${statusBadge(g.status)}</td>
        </tr>
      `).join('');
    }

    const totalPages = Math.ceil(filtered.length / PAGE_SIZE) || 1;
    document.getElementById('page-info').textContent = `Page ${page + 1} of ${totalPages} (${filtered.length} games)`;
    document.getElementById('prev-page').disabled = page === 0;
    document.getElementById('next-page').disabled = page >= totalPages - 1;
  }

  function updateSummary(data) {
    const counts = data.reduce((acc, g) => {
      acc.total++;
      if (g.status === 'ok') acc.ok++;
      else acc.review++;
      if (g.status === 'blank') acc.blank++;
      return acc;
    }, { total: 0, ok: 0, review: 0, blank: 0 });

    const percent = counts.total ? (counts.ok / counts.total) * 100 : 0;
    const setText = (id, value) => {
      const el = document.getElementById(id);
      if (el) el.textContent = value;
    };

    setText('summary-total', formatNumber(counts.total));
    setText('summary-ok', formatNumber(counts.ok));
    setText('summary-review', formatNumber(counts.review));
    setText('summary-blank', formatNumber(counts.blank));
    setText('legend-ok', formatNumber(counts.ok));
    setText('legend-review', formatNumber(counts.review));
    setText('legend-blank', formatNumber(counts.blank));

    const fill = document.getElementById('compat-bar-fill');
    if (fill) fill.style.width = `${percent.toFixed(1)}%`;
  }

  function applyFilters() {
    page = 0;
    filtered = allGames.filter(g => {
      const q = searchQuery.toLowerCase();
      if (q && !g.title.toLowerCase().includes(q)) return false;
      if (activeFilter === 'ok' && g.status !== 'ok') return false;
      if (activeFilter === 'review' && g.status === 'ok') return false;
      if (activeFilter === 'blank' && g.status !== 'blank') return false;
      if (activeFilter === 'dmg' && g.platform !== 'DMG') return false;
      if (activeFilter === 'gbc' && !g.platform.startsWith('GBC')) return false;
      return true;
    });
    renderTable();
  }

  fetch('compat_data.json')
    .then(r => r.json())
    .then(data => {
      allGames = data;
      filtered = data;
      updateSummary(data);
      renderTable();

      document.getElementById('search').addEventListener('input', e => {
        searchQuery = e.target.value;
        applyFilters();
      });

      document.querySelectorAll('.filter-btn').forEach(btn => {
        btn.addEventListener('click', () => {
          document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
          btn.classList.add('active');
          activeFilter = btn.dataset.filter;
          searchQuery = '';
          document.getElementById('search').value = '';
          applyFilters();
        });
      });

      document.getElementById('prev-page').addEventListener('click', () => {
        if (page > 0) { page--; renderTable(); }
      });

      document.getElementById('next-page').addEventListener('click', () => {
        const totalPages = Math.ceil(filtered.length / PAGE_SIZE);
        if (page < totalPages - 1) { page++; renderTable(); }
      });
    })
    .catch(() => {
      document.getElementById('compat-empty').classList.remove('hidden');
      document.getElementById('compat-empty').textContent = 'Could not load compatibility data.';
    });
})();
