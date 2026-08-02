(function() {
  const SUPPORTED = ['.cue', '.chd', '.iso', '.img', '.bin', '.exe', '.ps-exe', '.psexe', '.zip'];
  const PRIORITY = ['.cue', '.chd', '.iso', '.img', '.bin', '.exe', '.ps-exe', '.psexe', '.zip'];
  let importSequence = 0;

  function reportError(error) {
    const message = error && error.message ? error.message : String(error || 'Unknown browser file error');
    console.error('ARMSX browser file access failed', error);
    if (typeof Module !== 'undefined' && typeof Module.ccall === 'function') {
      Module.ccall('psxe_wasm_on_error', null, ['string'], [message]);
    }
  }

  function extension(name) {
    const lower = name.toLowerCase();
    return SUPPORTED.find((candidate) => lower.endsWith(candidate)) || '';
  }

  function safePart(value) {
    const part = String(value || '').replaceAll('\\', '/').split('/').pop();
    if (!part || part === '.' || part === '..') throw new Error('Unsafe file name');
    return part.replace(/[\u0000-\u001f]/g, '_');
  }

  function filesystem() {
    const result = typeof FS !== 'undefined' ? FS : Module.FS;
    if (!result) throw new Error('ARMSX filesystem is not ready');
    return result;
  }

  async function stageFile(file, destination) {
    const fs = filesystem();
    const parent = destination.substring(0, destination.lastIndexOf('/')) || '/';
    fs.mkdirTree(parent);
    const reader = file.stream().getReader();
    const handle = fs.open(destination, 'w');
    let offset = 0;
    try {
      for (;;) {
        const chunk = await reader.read();
        if (chunk.done) break;
        fs.write(handle, chunk.value, 0, chunk.value.byteLength, offset);
        offset += chunk.value.byteLength;
      }
    } finally {
      fs.close(handle);
      reader.releaseLock();
    }
  }

  function bestCandidate(paths) {
    return paths
      .filter((path) => extension(path))
      .sort((left, right) => PRIORITY.indexOf(extension(left)) - PRIORITY.indexOf(extension(right)))[0] || '';
  }

  async function stageEntries(entries) {
    const root = '/web-import/session-' + (++importSequence);
    const staged = [];
    for (const entry of entries) {
      const relative = entry.relative.split('/').map(safePart).join('/');
      const destination = root + '/' + relative;
      await stageFile(entry.file, destination);
      staged.push(destination);
    }
    const candidate = bestCandidate(staged);
    if (!candidate) throw new Error('No supported PlayStation image or executable was selected');
    Module.ccall('psxe_wasm_on_file', null, ['string'], [candidate]);
  }

  function inputSelection(directory) {
    return new Promise((resolve) => {
      const input = document.createElement('input');
      input.type = 'file';
      input.multiple = true;
      input.accept = SUPPORTED.join(',');
      if (directory) input.setAttribute('webkitdirectory', '');
      input.style.display = 'none';
      input.addEventListener('change', () => {
        const entries = Array.from(input.files || []).map((file) => ({
          file,
          relative: file.webkitRelativePath || file.name
        }));
        input.remove();
        resolve(entries);
      }, { once: true });
      document.body.appendChild(input);
      input.click();
    });
  }

  async function walkDirectory(handle, prefix, entries) {
    for await (const [name, child] of handle.entries()) {
      const relative = prefix ? prefix + '/' + safePart(name) : safePart(name);
      if (child.kind === 'directory') {
        await walkDirectory(child, relative, entries);
      } else if (child.kind === 'file') {
        entries.push({ file: await child.getFile(), relative });
      }
    }
  }

  function promptFor(label, action) {
    const existing = document.getElementById('armsx-file-prompt');
    if (existing) existing.remove();
    const overlay = document.createElement('div');
    overlay.id = 'armsx-file-prompt';
    overlay.style.cssText = 'position:fixed;inset:0;z-index:10000;display:grid;place-items:center;background:rgba(0,0,0,.72)';
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.style.cssText = 'font:600 18px system-ui;padding:16px 24px;border:1px solid #fff;background:#252525;color:#fff;border-radius:6px';
    button.addEventListener('click', async () => {
      overlay.remove();
      await action();
    }, { once: true });
    overlay.addEventListener('click', (event) => {
      if (event.target === overlay) overlay.remove();
    });
    overlay.appendChild(button);
    document.body.appendChild(overlay);
    button.focus();
  }

  window.ARMSXWebFiles = {
    openFiles() {
      promptFor('Choose PlayStation files', async () => {
      try {
        let entries;
        if (typeof window.showOpenFilePicker === 'function') {
          const handles = await window.showOpenFilePicker({
            multiple: true,
            types: [{
              description: 'PlayStation games',
              accept: { 'application/octet-stream': SUPPORTED }
            }]
          });
          entries = [];
          for (const handle of handles) {
            entries.push({ file: await handle.getFile(), relative: handle.name });
          }
        } else {
          entries = await inputSelection(false);
        }
        if (entries.length) await stageEntries(entries);
      } catch (error) {
        if (!error || error.name !== 'AbortError') reportError(error);
      }
      });
    },
    openDirectory() {
      promptFor('Choose a game folder', async () => {
      try {
        let entries = [];
        if (typeof window.showDirectoryPicker === 'function') {
          const handle = await window.showDirectoryPicker({ mode: 'read' });
          await walkDirectory(handle, safePart(handle.name), entries);
        } else {
          entries = await inputSelection(true);
        }
        if (entries.length) await stageEntries(entries);
      } catch (error) {
        if (!error || error.name !== 'AbortError') reportError(error);
      }
      });
    },
    _test: { extension, bestCandidate, safePart }
  };
})();
