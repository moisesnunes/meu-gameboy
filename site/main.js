// Navbar scroll effect
const navbar = document.getElementById('navbar');
window.addEventListener('scroll', () => {
  navbar.style.boxShadow = window.scrollY > 10
    ? '0 2px 20px rgba(0,0,0,0.4)'
    : 'none';
}, { passive: true });

// Animate stat counters on hero enter
const counters = document.querySelectorAll('.stat-num[id]');
const observer = new IntersectionObserver((entries) => {
  entries.forEach(entry => {
    if (!entry.isIntersecting) return;
    const el = entry.target;
    const target = parseInt(el.textContent.replace(/\D/g, ''));
    let current = 0;
    const step = Math.ceil(target / 60);
    const timer = setInterval(() => {
      current = Math.min(current + step, target);
      el.textContent = current.toLocaleString();
      if (current >= target) clearInterval(timer);
    }, 16);
    observer.unobserve(el);
  });
}, { threshold: 0.5 });

counters.forEach(el => observer.observe(el));
