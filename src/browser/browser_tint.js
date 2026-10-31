/* Report the colour the page shows at its top — the root's background, else
 * the body's, else the canvas — so a device's status bar can be painted in it
 * as a phone's browser paints it. Runs in a private world: the page cannot
 * call the handler or see this code. */
(function () {
  "use strict";
  if (window.top !== window) return;
  var last = -1, timer = 0, mo = null, body = null;
  var attrs = { attributes: true, attributeFilter: ["class", "style", "data-theme", "data-color-scheme"] };

  function rgbOf(el) {
    if (!el) return -1;
    var m = /^rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*([\d.]+))?\)$/.exec(getComputedStyle(el).backgroundColor);
    if (!m || (m[4] !== undefined && parseFloat(m[4]) < 0.5)) return -1;
    return (Math.min(255, +m[1]) << 16) | (Math.min(255, +m[2]) << 8) | Math.min(255, +m[3]);
  }

  function canvas() {
    /* A page that opted into a dark scheme gets a dark canvas while the
     * system is dark; any other page a white one. */
    var scheme = getComputedStyle(document.documentElement).colorScheme || "";
    var dark = window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;
    return scheme.indexOf("dark") >= 0 && (dark || scheme.indexOf("light") < 0) ? 0x000000 : 0xffffff;
  }

  function report() {
    timer = 0;
    if (!document.documentElement) return;
    var v = rgbOf(document.documentElement);
    if (v < 0) v = rgbOf(document.body);
    if (v < 0) v = canvas();
    if (v === last) return;
    last = v;
    try { window.webkit.messageHandlers.liuTint.postMessage({ rgb: v }); } catch (e) {}
  }

  function soon() {
    /* A body that arrived, or replaced the one watched, is watched too. */
    if (mo && document.body && document.body !== body) {
      body = document.body;
      mo.observe(body, attrs);
    }
    if (!timer) timer = setTimeout(report, 120);
  }

  function watch() {
    mo = new MutationObserver(soon);
    /* One observe() per node: a second call on the same node replaces the
     * first one's options. A stylesheet or a <style> added later recolours
     * without touching an attribute, so children are watched too. */
    mo.observe(document.documentElement, {
      attributes: true, attributeFilter: attrs.attributeFilter, childList: true });
    if (document.head) mo.observe(document.head, { childList: true });
    soon();
    setTimeout(report, 1000);
    if (window.matchMedia) {
      var mq = window.matchMedia("(prefers-color-scheme: dark)");
      if (mq.addEventListener) mq.addEventListener("change", soon);
    }
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", watch);
  else watch();
  window.addEventListener("load", soon);
  document.addEventListener("transitionend", soon, true);
})();
