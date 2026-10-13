const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

async function main() {
    const timers = new Map();
    const elements = [];
    let nextTimer = 0;
    const blocks = [];
    const root = { querySelectorAll: () => blocks };
    const themes = [];
    let releaseRun;
    let runs = 0;
    const window = {
        location: { search: '' },
        setTimeout: (fn, delay) => { const id = ++nextTimer; timers.set(id, { fn, delay }); return id; },
        clearTimeout: id => timers.delete(id),
        mermaid: {
            initialize: options => themes.push(options.theme),
            run: async ({ nodes }) => {
                runs += 1;
                if (runs === 1) await new Promise(resolve => { releaseRun = resolve; });
                for (const node of nodes) node.innerHTML = '<svg>' + themes.at(-1) + '</svg>';
            }
        },
        __mrrStampKeep: node => node.attrs.add('data-mrr-keep')
    };
    const document = {
        readyState: 'loading',
        addEventListener: () => {},
        createElement: () => ({}),
        head: { appendChild: element => { elements.push(element); if (element.src) element.onload(); } },
        getElementById: () => root
    };
    let source = fs.readFileSync(path.join(__dirname, '../../tools/js/mrr_markdown.js'), 'utf8');
    source = source.trimEnd().replace('}());', 
        'window.test = { loadStyle, ensureMath, ensureMermaid, prepareDiagramTheme, scheduleDiagrams, ' +
        'setOptions: value => { renderOptions = value; }, queue: () => diagramQueue }; }());');
    vm.runInNewContext(source, { window, document, Promise, console });
    const api = window.test;
    const flush = async () => { for (let i = 0; i < 20; ++i) await Promise.resolve(); };
    const fire = delay => {
        for (const [id, timer] of [...timers]) {
            if (timer.delay === delay) { timers.delete(id); timer.fn(); }
        }
    };

    let settled = false;
    api.loadStyle('https://blocked.invalid/katex.css').then(() => { settled = true; });
    await flush();
    assert.equal(settled, false);
    const lateLoad = elements[0].onload;
    fire(8000);
    await flush();
    assert.equal(settled, true);
    assert.equal(elements[0].onload, null);
    lateLoad(); // late network completion must be harmless
    assert.equal(timers.size, 0);

    window.katex = {};
    window.renderMathInElement = () => {};
    api.setOptions({ allowRemote: true, katexVersion: "0.16.22", mermaidVersion: "11.4.1", cdnBase: "https://cdn.invalid" });
    let mathDone = false;
    api.ensureMath().then(() => { mathDone = true; });
    await flush();
    assert.equal(mathDone, false);
    fire(8000); // hanging CSS must not prevent script loading / math readiness
    await flush();
    assert.equal(mathDone, true);
    assert.equal(window.__mrrMarkdownTestHooks.assetState().math, 'ready');

    const block = {
        isConnected: true, innerHTML: 'graph TD; A-->B', attrs: new Set(),
        removeAttribute: function (name) { this.attrs.delete(name); }
    };
    blocks.push(block);
    api.prepareDiagramTheme(root);
    api.scheduleDiagrams();
    fire(400);
    await flush();
    assert.equal(runs, 1);
    api.setOptions({ allowRemote: true, dark: true });
    block.attrs.add('data-mrr-keep');
    block.attrs.add('data-processed');
    api.prepareDiagramTheme(root);
    assert.equal(block.attrs.size, 0);
    block.innerHTML = 'graph TD; A-->B'; // raw source supplied by DOM morph
    api.scheduleDiagrams();
    fire(400);
    await flush();
    assert.equal(runs, 1); // next theme waits for the previous asynchronous run
    releaseRun();
    await api.queue();
    assert.equal(runs, 2);
    assert.equal(themes.at(-1), 'dark');
    assert.equal(block.innerHTML, '<svg>dark</svg>');
    assert.equal(block.attrs.has('data-mrr-keep'), true);
    console.log('Markdown assets: CSS timeout, math readiness, theme change and overlapping jobs passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
