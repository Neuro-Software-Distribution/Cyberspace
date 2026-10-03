// Bruh the (() => {} syntax is just enough to give up on JS why people still uses this? 


(() => {
    const content = document.getElementById('page-content');
    const status = document.getElementById('navigation-status');
    if (!content || !status) return;

    let loginEnabled = false;
    const updateSignOut = () => document.querySelectorAll('.session-controls')
        .forEach(form => { form.hidden = !loginEnabled; });
    fetch('/auth/status', { cache: 'no-store' }).then(response => response.json())
        .then(result => { loginEnabled = result.enabled === true; updateSignOut(); }).catch(() => {});
    document.addEventListener('pagechange', updateSignOut);

    let busy = false;
    let pendingPop = false;
    let displayedUrl = window.location.href;

    function isPage(url) {
        return url.origin === window.location.origin &&
            (url.pathname === '/' || url.pathname === '/404.html' || /^\/todos\/\d+$/.test(url.pathname));
    }

    function saveScroll() {
        history.replaceState({ ...history.state, scrollY: window.scrollY }, '', window.location.href);
    }

    function sameDocument(left, right) {
        return left.origin === right.origin && left.pathname === right.pathname && left.search === right.search;
    }

    function scrollToFragment(url) {
        let id;
        try { id = decodeURIComponent(url.hash.slice(1)); }
        catch { id = url.hash.slice(1); }
        if (!id) { window.scrollTo(0, 0); return; }
        const target = document.getElementById(id);
        if (!target || !content.contains(target)) return;
        target.setAttribute('tabindex', '-1');
        target.focus({ preventScroll: true });
        target.scrollIntoView();
    }

    // Keep the outgoing entry's position ready before Back/Forward changes it.
    window.addEventListener('scroll', () => {
        if (!busy && displayedUrl === window.location.href) saveScroll();
    }, { passive: true });

    saveScroll();
    history.scrollRestoration = 'manual';

    async function navigate(url, { body, pop = false } = {}) {
        if (busy) return;
        busy = true;
        const previousScroll = window.scrollY;
        const restoreScroll = history.state?.scrollY || 0;
        if (!pop) saveScroll();
        content.setAttribute('aria-busy', 'true');
        status.hidden = false;
        status.textContent = body ? 'Saving…' : 'Loading…';

        try {
            const response = await fetch(url, {
                method: body ? 'POST' : 'GET',
                body,
                credentials: 'same-origin',
                cache: 'no-store',
                signal: AbortSignal.timeout(15000),
            });
            if (response.status === 401 || new URL(response.url).pathname === '/login') {
                window.location.assign('/login');
                return;
            }
            if (!response.ok && response.status !== 404) throw new Error(`Server returned ${response.status}`);
            const destination = new URL(response.url);
            if (!response.redirected) destination.hash = url.hash;
            const page = new DOMParser().parseFromString(await response.text(), 'text/html');
            const replacement = page.getElementById('page-content');
            if (destination.origin !== window.location.origin ||
                (!isPage(destination) && response.status !== 404) || !replacement) {
                throw new Error('Unexpected page response');
            }
            
            if (pendingPop) return;

            const samePage = destination.pathname === new URL(displayedUrl).pathname;
            if (!pop && destination.href !== window.location.href) {
                history.pushState({ scrollY: 0 }, '', destination);
            }
            content.replaceChildren(...replacement.childNodes);
            document.title = page.title;
            displayedUrl = destination.href;
            document.dispatchEvent(new Event('pagechange'));
            window.Prism?.highlightAllUnder(content);
            status.hidden = true;

            const main = content.querySelector('main');
            main?.setAttribute('tabindex', '-1');
            main?.focus({ preventScroll: true });
            window.scrollTo(0, pop ? restoreScroll : body && samePage ? previousScroll : 0);
            if (!pop && destination.hash) {
                scrollToFragment(destination);
            }
        } catch (error) {
            if (!pendingPop) {
                status.textContent = body
                    ? 'Could not confirm the save. Reopen the home page or todo to check before submitting again.'
                    : 'Could not load that page. Check your connection and try the link again.';
                console.warn('Page navigation failed', error);
                if (pop) history.replaceState({ scrollY: previousScroll }, '', displayedUrl);
            }
        } finally {
            busy = false;
            content.removeAttribute('aria-busy');
            if (pendingPop) {
                pendingPop = false;
                navigate(new URL(window.location.href), { pop: true });
            }
        }
    }

    document.addEventListener('click', event => {
        if (event.defaultPrevented || event.button !== 0 || event.metaKey || event.ctrlKey ||
            event.shiftKey || event.altKey) return;
        const link = event.target.closest('a[href]');
        if (!link || !content.contains(link) || link.hasAttribute('download') ||
            (link.target && link.target !== '_self')) return;
        const url = new URL(link.href);
        if (!isPage(url)) return;
        if (sameDocument(url, new URL(window.location.href)) && link.href.includes('#')) {
            event.preventDefault();
            if (busy) return;
            saveScroll();
            if (url.href !== window.location.href) history.pushState({}, '', url);
            displayedUrl = url.href;
            scrollToFragment(url);
            saveScroll();
            return;
        }
        event.preventDefault();
        navigate(url);
    });

    document.addEventListener('submit', async event => {
        const form = event.target;
        if (event.defaultPrevented || !content.contains(form)) return;
        const submitter = event.submitter;
        const url = new URL(submitter?.getAttribute('formaction') || form.action, window.location.href);
        const method = submitter?.getAttribute('formmethod') || form.method;
        const target = submitter?.getAttribute('formtarget') || form.target;
        if (url.origin !== window.location.origin || method.toLowerCase() !== 'post' ||
            (target && target !== '_self') || !['/', '/complete', '/update', '/delete', '/folders', '/folders/move'].includes(url.pathname)) return;
        event.preventDefault();
        if (busy) return;
        const data = new FormData(form);
        if (submitter?.name) data.append(submitter.name, submitter.value);
        if (url.pathname === '/delete') {
            const dialog = content.querySelector('.nuke-dialog');
            if (!dialog || dialog.open) return;
            dialog.returnValue = 'cancel';
            const confirmed = new Promise(resolve => {
                dialog.addEventListener('close', () => resolve(dialog.returnValue === 'nuke'), { once: true });
            });
            dialog.showModal();
            if (!await confirmed || !form.isConnected || busy) return;
        }
        // The C handlers expect URL-encoded fields, not multipart FormData.
        navigate(url, { body: new URLSearchParams(data) });
    });

    window.addEventListener('popstate', () => {
        content.querySelector('.nuke-dialog[open]')?.close('cancel');
        if (busy) pendingPop = true;
        else {
            const url = new URL(window.location.href);
            if (sameDocument(url, new URL(displayedUrl))) {
                displayedUrl = url.href;
                if (Number.isFinite(history.state?.scrollY)) window.scrollTo(0, history.state.scrollY);
                else scrollToFragment(url);
            } else navigate(url, { pop: true });
        }
    });
})();
