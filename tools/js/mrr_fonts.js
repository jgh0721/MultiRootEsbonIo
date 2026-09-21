(function () {
    "use strict";
    if (window.__mrrPreviewFonts) return;

    const protectedSelector = "svg,math,mjx-container,.MathJax,.MathJax_Display,.katex,.math," +
        ".mermaid,.graphviz,.fa,.fas,.far,.fab,.material-icons,.material-symbols-outlined," +
        "[class*='icon'],[class*='Icon']";
    const headings = "h1,h2,h3,h4,h5,h6";
    const code = "pre,code,kbd,samp";
    const style = document.createElement("style");
    style.id = "mrr-preview-fonts";
    let options = null;
    let pending = false;
    let saved = new Map();

    function patch(element, property, value) {
        let properties = saved.get(element);
        if (!properties) saved.set(element, properties = new Map());
        if (!properties.has(property)) properties.set(property, {
            value: element.style.getPropertyValue(property),
            priority: element.style.getPropertyPriority(property), applied: value
        });
        element.style.setProperty(property, value, "important");
        properties.get(property).applied = element.style.getPropertyValue(property);
    }

    function restore() {
        saved.forEach((properties, element) => properties.forEach((old, property) => {
            // A DOM morph may already have replaced the document's inline style.
            if (element.style.getPropertyValue(property) !== old.applied ||
                element.style.getPropertyPriority(property) !== "important") return;
            if (old.value) element.style.setProperty(property, old.value, old.priority);
            else element.style.removeProperty(property);
        }));
        saved.clear();
        style.remove();
    }

    function update() {
        pending = false;
        observer.disconnect();
        restore();
        if (!document.body || !options || !options.enabled || options.mode === "document") return;

        const family = JSON.stringify(options.bodyFamily || "serif");
        const codeFamily = JSON.stringify(options.codeFamily || "monospace");
        const size = options.bodySize + "px";
        const codeSize = options.codeSize + "px";
        const line = String(options.lineHeight);
        const excluded = protectedSelector + "," + protectedSelector.split(",").map(s => s + " *").join(",");
        const eligible = ":not(:is(" + excluded + "))";
        const ordinary = eligible + ":not(:is(" + code + ",pre *,code *,kbd *,samp *," +
            headings + ",h1 *,h2 *,h3 *,h4 *,h5 *,h6 *))";
        // Preserve inherited document fonts on headings and specialised renderers.
        document.querySelectorAll(protectedSelector + (!options.headings ? "," + headings : ""))
            .forEach(element => {
                if (element.parentElement && element.parentElement.closest(protectedSelector)) return;
                const computed = getComputedStyle(element);
                const original = [computed.fontFamily, computed.fontSize, computed.lineHeight];
                patch(element, "font-family", original[0]);
                if (element.matches(protectedSelector)) {
                    patch(element, "font-size", original[1]);
                    patch(element, "line-height", original[2]);
                }
            });

        const bodyTargets = "body,main,article,section,div,p,li,td,th,dt,dd,blockquote";
        const inlineTargets = "span,a,strong,em,b,i,label";
        // Important declarations in a layer take precedence over unlayered theme rules.
        style.textContent = "@layer mrr-preview-fonts {" +
            "body" + ordinary + ",body *" + ordinary + "{font-family:" + family + " !important;}" +
            ":is(" + bodyTargets + ")" + ordinary + "{font-size:" + size + " !important;line-height:" + line + " !important;}" +
            ":is(" + inlineTargets + ")" + ordinary + "{font-size:inherit !important;line-height:inherit !important;}" +
            ":is(" + code + ",pre *,code *,kbd *,samp *)" + eligible + "{font-family:" + codeFamily +
                " !important;font-size:" + codeSize + " !important;line-height:" + line + " !important;}" +
            (options.headings ? ":is(" + headings + ",h1 *,h2 *,h3 *,h4 *,h5 *,h6 *)" + eligible +
                "{font-family:" + family + " !important;}" : "") + "}";

        // Inline !important beats every stylesheet. Override only those declarations,
        // retaining their original values so switching back is lossless.
        document.querySelectorAll("body[style],body [style]").forEach(element => {
            if (element.closest(protectedSelector)) return;
            const heading = element.closest(headings);
            if (heading && !options.headings) return;
            const isCode = !!element.closest(code);
            const values = { "font-family": isCode ? codeFamily : family };
            if (!heading) {
                values["font-size"] = isCode ? codeSize : size;
                values["line-height"] = line;
            }
            Object.keys(values).forEach(property => {
                if (element.style.getPropertyPriority(property) === "important")
                    patch(element, property, values[property]);
            });
        });
        // First layer wins among !important declarations, including layered themes.
        const host = document.head || document.documentElement;
        host.insertBefore(style, host.firstChild);
        observer.observe(document.documentElement, {childList: true, subtree: true, attributes: true,
            attributeFilter: ["style", "class"]});
    }

    const observer = new MutationObserver(() => {
        if (pending) return;
        pending = true;
        // Hidden previews do not receive animation frames. Coalesce DOM changes
        // without depending on visibility (tab switches and headless tests too).
        queueMicrotask(update);
    });
    window.__mrrPreviewFonts = {apply: function (value) { options = value; update(); }};
})();
