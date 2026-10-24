/* Native start/error controls. Runs only in Liu's private WKContentWorld. */
(function (requestToken) {
  "use strict";
  document.addEventListener("click", function (e) {
    // The DOM is shared with page scripts; synthetic events carry no authority.
    if (!e.isTrusted) return;
    var target = e.target && e.target.closest ? e.target : null;
    if (!target) return;
    var ask = target.closest("#ask");
    var show = target.closest("#show");
    if (ask) {
      if (ask.disabled) return;
      window.webkit.messageHandlers.liuBrowser.postMessage({ type: "askAgent", requestToken: requestToken });
      ask.textContent = "Asked agent \u2713";
      ask.disabled = true;
    } else if (show) {
      var details = document.getElementById("details");
      if (!details) return;
      var open = details.style.display === "block";
      details.style.display = open ? "none" : "block";
      show.textContent = open ? "Show Details" : "Hide Details";
    } else {
      var row = target.closest("[data-url]");
      var url = row && row.getAttribute("data-url");
      if (url) window.webkit.messageHandlers.liuBrowser.postMessage({ type: "navigate", url: url, requestToken: requestToken });
    }
  });
})
