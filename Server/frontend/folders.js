(() => {
    const openFolders = new Set();
    let dragged = null;
    const content = document.getElementById('page-content');
    if (!content) return;

    document.addEventListener('toggle', event => {
        const card = event.target.closest('.folder-card');
        if (!card || !content.contains(card)) return;
        if (event.target.open) openFolders.add(card.dataset.folderId);
        else openFolders.delete(card.dataset.folderId);
    }, true);

    document.addEventListener('pagechange', () => {
        dragged = null;
        content.querySelectorAll('.folder-card').forEach(card => {
            card.querySelector('details').open = openFolders.has(card.dataset.folderId);
        });
    });

    function clearTargets() {
        content.querySelectorAll('.is-drop-target').forEach(target => target.classList.remove('is-drop-target'));
    }

    document.addEventListener('dragstart', event => {
        const item = event.target.closest('.todo-item');
        if (!item || !content.contains(item) || content.hasAttribute('aria-busy') ||
            event.target.closest('button, select, input')) {
            if (item) event.preventDefault();
            return;
        }
        dragged = item;
        event.dataTransfer.setData('text/plain', item.dataset.todoId);
        event.dataTransfer.effectAllowed = 'move';
        item.classList.add('is-dragging');
    });

    document.addEventListener('dragover', event => {
        if (!dragged) return;
        const target = event.target.closest('[data-folder-id]');
        clearTargets();
        if (!target || !content.contains(target)) return;
        event.preventDefault();
        event.dataTransfer.dropEffect = 'move';
        target.classList.add('is-drop-target');
    });

    document.addEventListener('dragleave', event => {
        const target = event.target.closest('[data-folder-id]');
        if (target && !target.contains(event.relatedTarget)) target.classList.remove('is-drop-target');
    });

    document.addEventListener('drop', event => {
        const target = event.target.closest('[data-folder-id]');
        clearTargets();
        if (!dragged || !dragged.isConnected || !target || !content.contains(target)) return;
        event.preventDefault();
        const form = dragged.querySelector('.todo-move');
        form.elements.folder_id.value = target.dataset.folderId;
        form.requestSubmit();
    });

    document.addEventListener('dragend', () => {
        dragged?.classList.remove('is-dragging');
        dragged = null;
        clearTargets();
    });
})();
