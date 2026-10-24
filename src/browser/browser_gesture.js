/* Authorize one link handoff from a real user event, in a private world. */
(function () {
  "use strict";
  function record(e) {
    if (!e.isTrusted) return;
    /* The link nearest the real target, across shadow roots: a window
     * listener is told the shadow HOST, so a link inside a web component
     * carried no gesture — a public one got the "not allowed" page and a
     * target=_blank one did nothing. The composed path also still reaches a
     * link that wraps the component. */
    var path = e.composedPath ? e.composedPath() : [e.target];
    var link = null;
    for (var i = 0; i < path.length && !link; i++) {
      var node = path[i];
      if (node && node.nodeType === 1 && node.matches("a,area")) link = node;
    }
    if (!link) return;
    try {
      var href = link.getAttribute("href") || link.getAttributeNS("http://www.w3.org/1999/xlink", "href");
      if (!href) return;
      var url = new URL(href, document.baseURI);
      if (url.protocol !== "http:" && url.protocol !== "https:") return;
      var bytes = new Uint32Array(4);
      crypto.getRandomValues(bytes);
      window.__liuGestureToken = Array.from(bytes, function (n) { return n.toString(16).padStart(8, "0"); }).join("");
      window.webkit.messageHandlers.liuGesture.postMessage({ url: url.href, requestToken: window.__liuGestureToken });
    } catch (error) {}
  }
  window.addEventListener("click", record, true);
  if ("onauxclick" in window) window.addEventListener("auxclick", record, true);
  else window.addEventListener("mouseup", function (e) { if (e.button === 1) record(e); }, true);
})();
