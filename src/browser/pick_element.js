/* Liu Browser — Select Element user script (injected at document start). */
(function () {
  "use strict";
  if (window.__liuPickInstalled) return;
  window.__liuPickInstalled = true;

  var mode = false;
  var requestToken = "";  // native arm, private to Liu's WKContentWorld
  var lastEl = null;

  /* --- Inspector theme. Defaults reproduce the classic dark look; the app
   *     overrides every entry from the ACTIVE terminal theme via
   *     __liuPickSetTheme (called by the native side when picking starts),
   *     so the overlay + tooltip follow Liu theming. --- */
  var theme = {
    accent:    "59,130,246",             /* highlight box + pulse, "r,g,b" */
    tipBg:     "#121212",                /* tooltip background */
    tipFg:     "#e5e7eb",                /* tooltip base text (px size) */
    tipBorder: "rgba(82,53,97,1)",       /* tooltip border */
    tagColor:  "#7dd3fc",                /* element tag name */
    clsColor:  "#fca5a5",                /* .classes */
    idColor:   "#fcd34d",                /* #id */
    dimColor:  "rgba(148,163,184,.75)"   /* divider */
  };

  /* --- Inspector overlay: an animated highlight box + a tooltip that reads
   *     "tag.class | W × H". Built lazily, mounted once, never mutates the
   *     inspected element's own styles (an outline on the element itself
   *     could get left behind and disturb layout). All parts are
   *     pointer-events:none and carry data-liu-pick so hit-testing skips
   *     them. --- */
  var ovRoot = null, ovBox = null, ovTip = null, ovCursor = null, ovStyle = null;

  /* (Re)stamp the current theme onto the mounted overlay: the pulse keyframes
   * carry baked-in colors, so they are rebuilt from the accent each time. */
  function applyTheme() {
    if (!ovRoot) return;
    var a = theme.accent;
    ovStyle.textContent =
      "@keyframes liuPickPulse{" +
      "0%{box-shadow:0 0 0 0 rgba(" + a + ",.55);outline-color:rgba(" + a + ",1)}" +
      "70%{box-shadow:0 0 0 6px rgba(" + a + ",0);outline-color:rgba(" + a + ",.65)}" +
      "100%{box-shadow:0 0 0 0 rgba(" + a + ",0);outline-color:rgba(" + a + ",1)}}" +
      "@keyframes liuPickTipIn{from{opacity:0;transform:translateY(4px)}" +
      "to{opacity:1;transform:translateY(0)}}";
    ovBox.style.background = "rgba(" + a + ",.14)";
    ovBox.style.outlineColor = "rgba(" + a + ",1)";
    ovTip.style.color = theme.tipFg;
    ovTip.style.background = theme.tipBg;
    ovTip.style.borderColor = theme.tipBorder;
    if (mode && lastEl) showOverlay(lastEl);   /* refresh tooltip span colors */
  }

  window.__liuPickSetTheme = function (t) {
    if (!t) return;
    for (var k in theme)
      if (Object.prototype.hasOwnProperty.call(theme, k) && t[k]) theme[k] = t[k];
    tipW = tipH = 0; lastW = lastH = -1;   /* force a tooltip rebuild+remeasure */
    applyTheme();
  };

  /* Find the nearest scroll container under a point so the crosshair overlay can
   * forward wheel scrolling to the page it covers. */
  function scrollableUnder(x, y) {
    var stack = document.elementsFromPoint(x, y);
    for (var i = 0; i < stack.length; i++) {
      var el = stack[i];
      if (isOverlay(el)) continue;
      var cur = el;
      while (cur && cur.nodeType === 1) {
        if (cur === document.body || cur === document.documentElement) break;
        var cs = null;
        try { cs = getComputedStyle(cur); } catch (e) {}
        if (cs) {
          var oy = cs.overflowY;
          if ((oy === "auto" || oy === "scroll" || oy === "overlay") &&
              cur.scrollHeight > cur.clientHeight + 1) return cur;
        }
        cur = cur.parentElement;
      }
      break;
    }
    return null;
  }

  /* The full-viewport crosshair overlay swallows pointer hit-testing, so page
   * wheel scrolling must be re-applied by hand. */
  function onCursorWheel(ev) {
    if (!mode) return;
    var unit = ev.deltaMode === 1 ? 16 : (ev.deltaMode === 2 ? 400 : 1);
    var dy = ev.deltaY * unit, dx = ev.deltaX * unit;
    var t = scrollableUnder(ev.clientX, ev.clientY);
    if (t) { t.scrollTop += dy; t.scrollLeft += dx; }
    else {
      var se = document.scrollingElement || document.documentElement;
      se.scrollTop += dy; se.scrollLeft += dx;
    }
    ev.preventDefault();
    onReflow();
  }

  function ensureOverlay() {
    if (ovRoot) return;
    ovStyle = document.createElement("style");
    ovStyle.setAttribute("data-liu-pick", "1");
    (document.head || document.documentElement).appendChild(ovStyle);

    ovRoot = document.createElement("div");
    ovRoot.setAttribute("data-liu-pick", "1");
    ovRoot.style.cssText =
      "position:fixed;left:0;top:0;width:0;height:0;z-index:2147483647;" +
      "pointer-events:none;";

    /* Full-viewport crosshair surface: because it sits on top and is
     * pointer-events:auto, the OS cursor over the whole page is forced to the
     * crosshair — page cursors (pointer/text/…) can't override a layer above
     * them. Transparent, so the page + highlight stay visible. Wheel scrolling
     * is forwarded (onCursorWheel) so the page still scrolls while picking. */
    ovCursor = document.createElement("div");
    ovCursor.setAttribute("data-liu-pick", "1");
    ovCursor.style.cssText =
      "position:fixed;inset:0;pointer-events:auto;cursor:crosshair;" +
      "background:transparent;display:none;";
    ovCursor.addEventListener("wheel", onCursorWheel, { passive: false });
    ovRoot.appendChild(ovCursor);

    ovBox = document.createElement("div");
    ovBox.setAttribute("data-liu-pick", "1");
    ovBox.style.cssText =
      "position:fixed;pointer-events:none;box-sizing:border-box;display:none;" +
      "border-radius:2px;" +
      "outline:2px solid;outline-offset:-1px;" +
      "animation:liuPickPulse 1.4s ease-out infinite;" +
      "transition:left .05s ease,top .05s ease,width .05s ease,height .05s ease;";

    ovTip = document.createElement("div");
    ovTip.setAttribute("data-liu-pick", "1");
    ovTip.style.cssText =
      "position:fixed;pointer-events:none;display:none;" +
      "font:600 11px/1.35 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;" +
      "border:1px solid;" +
      "padding:3px 7px;border-radius:5px;white-space:nowrap;" +
      "box-shadow:0 4px 16px rgba(0,0,0,.45);" +
      "animation:liuPickTipIn .12s ease-out;";

    ovRoot.appendChild(ovBox);
    ovRoot.appendChild(ovTip);
    (document.body || document.documentElement).appendChild(ovRoot);
    applyTheme();   /* colors (box/pulse/tooltip) all come from the theme */
  }

  function isOverlay(el) {
    if (!el) return false;
    if (el === ovRoot || el === ovBox || el === ovTip || el === ovCursor) return true;
    return !!(el.closest && el.closest("[data-liu-pick]"));
  }

  function tipSpan(text, color) {
    var s = document.createElement("span");
    s.textContent = text;
    s.style.color = color;
    return s;
  }

  /* Build the "tag.class | W × H" tooltip content for el. Span colors come
   * from the injected theme (terminal ANSI palette on the native side). */
  function fillTip(el, w, h) {
    ovTip.textContent = "";
    var tag = (el.tagName || "div").toLowerCase();
    ovTip.appendChild(tipSpan(tag, theme.tagColor));               /* element name */
    if (el.classList && el.classList.length) {
      var arr = [];
      for (var i = 0; i < el.classList.length && arr.length < 4; i++)
        arr.push(clip(el.classList[i], 64));
      ovTip.appendChild(tipSpan("." + arr.join("."), theme.clsColor)); /* classes */
    }
    if (el.id) ovTip.appendChild(tipSpan("#" + clip(el.id, 127), theme.idColor)); /* id */
    ovTip.appendChild(tipSpan("  |  ", theme.dimColor));           /* divider */
    ovTip.appendChild(tipSpan(w + " × " + h, theme.tipFg));        /* px size */
  }

  /* Tooltip content cache: rebuilding the spans + reading offsetWidth forces
   * a synchronous layout, too costly to run on every pointermove/scroll event
   * on a heavy page. Rebuild + remeasure only when the element (or its
   * rounded size) actually changed; pure moves and scroll-follow reuse the
   * cached measurement and stay layout-free. */
  var tipW = 0, tipH = 0, lastW = -1, lastH = -1;

  function showOverlay(el) {
    if (!el || el.nodeType !== 1) return;
    ensureOverlay();
    var r = el.getBoundingClientRect
      ? el.getBoundingClientRect()
      : { left: 0, top: 0, width: 0, height: 0 };
    var w = Math.round(r.width), h = Math.round(r.height);

    ovBox.style.display = "block";
    ovBox.style.left = r.left + "px";
    ovBox.style.top = r.top + "px";
    ovBox.style.width = Math.max(0, r.width) + "px";
    ovBox.style.height = Math.max(0, r.height) + "px";

    var rebuild = (el !== lastEl || w !== lastW || h !== lastH || !tipW);
    if (rebuild) {
      fillTip(el, w, h);
      lastW = w; lastH = h;
    }
    ovTip.style.display = "block";
    if (rebuild) {           /* measure once per content change, then reuse */
      tipW = ovTip.offsetWidth;
      tipH = ovTip.offsetHeight;
    }
    /* Place above the element; flip below when it would clip. */
    var tw = tipW, th = tipH;
    var vw = window.innerWidth, vh = window.innerHeight;
    var tx = r.left;
    var ty = r.top - th - 6;
    if (ty < 2) ty = r.top + r.height + 6;
    if (ty + th > vh - 2) ty = Math.max(2, vh - th - 2);
    if (tx + tw > vw - 2) tx = Math.max(2, vw - tw - 2);
    if (tx < 2) tx = 2;
    ovTip.style.left = tx + "px";
    ovTip.style.top = ty + "px";
    lastEl = el;
  }

  function hideOverlay() {
    if (ovBox) ovBox.style.display = "none";
    if (ovTip) ovTip.style.display = "none";
    lastEl = null;
    lastW = lastH = -1;      /* next show rebuilds + remeasures */
    tipW = tipH = 0;
  }

  function isHashedClass(c) {
    if (!c || c.length < 6) return false;
    if (c.indexOf("__") >= 0) return true;
    if (/^[a-zA-Z]+_[a-zA-Z0-9]{4,}$/.test(c)) return true;
    return false;
  }

  function deepElementFromPoint(x, y) {
    var stack = document.elementsFromPoint(x, y);
    if (!stack || !stack.length) return null;
    var el = null;
    for (var i = 0; i < stack.length; i++) {
      if (!isOverlay(stack[i])) { el = stack[i]; break; }  /* skip Liu's own overlay */
    }
    if (!el) return null;
    var cur = el;
    var guard = 0;
    while (cur && guard++ < 32) {
      if (cur.shadowRoot) {
        /* A shadow root hit-tests the whole document, so Liu's crosshair
         * layer (the whole viewport, on top) comes back first and would be
         * picked instead of the web component. Skip it as the document pass
         * does. */
        var root = cur.shadowRoot, inner = null;
        var list = root.elementsFromPoint ? root.elementsFromPoint(x, y) : [root.elementFromPoint(x, y)];
        for (var j = 0; list && j < list.length; j++) {
          if (list[j] && !isOverlay(list[j])) { inner = list[j]; break; }
        }
        if (inner && inner !== cur) { cur = inner; continue; }
      }
      break;
    }
    return cur || el;
  }

  function clip(value, cap) {
    var text = String(value || "");
    if (text.length <= cap) return text;
    if (cap && /[\uD800-\uDBFF]/.test(text.charAt(cap - 1)) &&
        /[\uDC00-\uDFFF]/.test(text.charAt(cap))) cap--;
    return text.slice(0, cap);
  }

  function cssPath(el) {
    var parts = [], cur = el;
    for (var depth = 0; cur && cur.nodeType === 1 && depth < 24; depth++) {
      var name = clip(cur.localName, 64);
      var id = cur.getAttribute("id");
      if (id && id.length <= 128) { parts.unshift(name + "#" + CSS.escape(id)); break; }
      var idx = 1, sibling = cur.previousElementSibling, budget = 128;
      while (sibling && budget-- > 0) {
        if (sibling.localName === cur.localName) idx++;
        sibling = sibling.previousElementSibling;
      }
      if (!sibling && idx > 1) name += ":nth-of-type(" + idx + ")";
      parts.unshift(name);
      cur = cur.parentElement;
    }
    return clip(parts.join(" > "), 1023);
  }

  function shortSelector(el) {
    var tag = clip(el.localName, 63);
    var id = el.getAttribute("id");
    if (id && id.length <= 128) return "#" + CSS.escape(id);
    /* The selector names the attribute the value came from: a data-cy value
     * written as [data-testid=…] matched nothing on a Cypress page. */
    var tattr = ["data-testid", "data-test", "data-cy"].filter(function (a) {
      return el.getAttribute(a);
    })[0];
    var testid = tattr ? el.getAttribute(tattr) : null;
    if (testid && testid.length <= 128)
      return "[" + tattr + '="' + CSS.escape(testid) + '"]';
    var cls = [];
    if (el.classList) {
      for (var i = 0; i < el.classList.length && i < 12 && cls.length < 3; i++) {
        var c = el.classList[i];
        if (c.length <= 64 && !isHashedClass(c)) cls.push(CSS.escape(c));
      }
    }
    return clip(cls.length ? tag + "." + cls.join(".") : tag || "element", 511);
  }

  /* Build a bounded snapshot directly. cloneNode(true), outerHTML, innerText
   * and querySelectorAll first traverse/copy the entire attacker-owned tree;
   * clipping their result does not bound that work. Visit at most 256 nodes,
   * 32 levels and 32 attributes per element, including rejected attributes. */
  function snapshot(el) {
    var html = "", text = "", nodes = 0, truncated = false;
    var voidTags = /^(area|base|br|col|embed|hr|img|input|link|meta|param|source|track|wbr)$/;
    function escaped(value) {
      return value.replace(/&/g, "&amp;").replace(/</g, "&lt;")
                  .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
    }
    function append(value) {
      var room = 8192 - html.length;
      if (value.length > room) truncated = true;
      html += clip(value, Math.max(0, room));
    }
    function visit(node, depth) {
      if (++nodes > 256 || depth > 32 || html.length >= 8192) { truncated = true; return; }
      if (node.nodeType === 3) {
        var part = node.substringData(0, 8192 - html.length);
        if (node.length > part.length) truncated = true;
        /* Never half a surrogate pair: native refuses text that is not valid
         * UTF-8, and the whole html field went with it. */
        var last = part.charCodeAt(part.length - 1);
        if (last >= 0xD800 && last <= 0xDBFF) part = part.slice(0, -1);
        append(escaped(part));
        if (text.length < 400) text += clip(part, 400 - text.length);
        return;
      }
      if (node.nodeType !== 1 || node.hasAttribute("data-liu-pick")) return;
      var tag = clip(node.localName, 64);
      if (!/^[a-zA-Z][a-zA-Z0-9:._-]{0,63}$/.test(tag)) tag = "div";
      append("<" + tag);
      var password = tag === "input" && node.type === "password";
      var attrs = node.attributes;
      if (attrs.length > 32) truncated = true;
      for (var i = 0; i < attrs.length && i < 32 && html.length < 8192; i++) {
        var attr = attrs[i], name = attr.name;
        if (password && name.toLowerCase() === "value") continue;
        if (name.length > 64 || !/^[a-zA-Z_:][a-zA-Z0-9:._-]*$/.test(name)) { truncated = true; continue; }
        var value = clip(attr.value, 1024);
        if (attr.value.length > value.length) truncated = true;
        append(" " + name + '=\"' + escaped(value) + '\"');
      }
      append(">");
      if (voidTags.test(tag)) return;
      var child = node.firstChild;
      while (child) {
        if (nodes >= 256 || html.length >= 8192 || depth >= 32) { truncated = true; break; }
        visit(child, depth + 1);
        child = child.nextSibling;
      }
      append("</" + tag + ">");
    }
    if (el) visit(el, 0);
    if (truncated) html += "<!-- truncated -->";
    return { html: html, text: text.replace(/\s+/g, " ").trim(), truncated: truncated };
  }

  function post(obj) {
    obj.requestToken = requestToken;
    try {
      if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.liuPick)
        window.webkit.messageHandlers.liuPick.postMessage(obj);
    } catch (e) {}
  }

  function buildPick(el) {
    var r = el.getBoundingClientRect ? el.getBoundingClientRect() : { x:0, y:0, width:0, height:0 };
    var testid = el.getAttribute("data-testid") || el.getAttribute("data-test") || el.getAttribute("data-cy") || "";
    var captured = snapshot(el);
    var lim = captured.truncated ? ["dom-budget"] : [];
    var tag = (el.tagName || "").toLowerCase();
    if (tag === "canvas") lim.push("canvas");
    var classes = "";
    if (el.classList) {
      var arr = [];
      for (var i = 0; i < el.classList.length && i < 12; i++) arr.push(clip(el.classList[i], 64));
      classes = arr.join(" ");
    }
    return {
      type: "pick",
      tag: tag,
      id: clip(el.id, 127),
      classes: clip(classes, 255),
      cssSelector: shortSelector(el),
      cssPath: cssPath(el),
      text: captured.text,
      ariaLabel: clip(el.getAttribute("aria-label"), 255),
      role: clip(el.getAttribute("role"), 63),
      html: captured.html,
      rect: { x: r.x, y: r.y, w: r.width, h: r.height },
      dataTestid: clip(testid, 127),
      url: clip(location.href, 2047),
      limitations: lim
    };
  }

  /* Coalesce pointermove/scroll bursts through requestAnimationFrame: the
   * hit-test (elementsFromPoint) and overlay update run at most once per
   * displayed frame instead of once per input event. */
  var moveRaf = false, pendX = 0, pendY = 0;
  function onMove(ev) {
    if (!mode) return;
    pendX = ev.clientX; pendY = ev.clientY;
    if (moveRaf) return;
    moveRaf = true;
    requestAnimationFrame(function () {
      moveRaf = false;
      if (!mode) return;
      var el = deepElementFromPoint(pendX, pendY);
      if (el) showOverlay(el);
    });
  }

  function onClick(ev) {
    if (!mode || !ev.isTrusted) return;
    ev.preventDefault();
    ev.stopPropagation();
    var el = deepElementFromPoint(ev.clientX, ev.clientY);
    if (!el) return;
    showOverlay(el);
    post(buildPick(el));
    window.__liuPickSetMode(false);
  }

  function onKey(ev) {
    if (!mode || !ev.isTrusted) return;
    if (ev.key === "Escape" || ev.keyCode === 27) {
      ev.preventDefault();
      post({ type: "cancel" });
      window.__liuPickSetMode(false);
    }
  }

  /* Keep the highlight glued to the element through scroll/resize —
   * rAF-coalesced like onMove (scroll events fire far above frame rate). */
  var reflowRaf = false;
  function onReflow() {
    if (!mode || !lastEl) return;
    if (reflowRaf) return;
    reflowRaf = true;
    requestAnimationFrame(function () {
      reflowRaf = false;
      if (!mode || !lastEl) return;
      if (lastEl.isConnected === false) { hideOverlay(); return; }
      showOverlay(lastEl);
    });
  }

  /* Force the crosshair (+) cursor over EVERY element while picking. Setting it
   * only on documentElement isn't enough — buttons/links/text carry their own
   * cursor (pointer/text) that would win, so inject a `!important` rule that
   * overrides them all. */
  function setPickCursor(on) {
    try {
      /* Primary force: a pointer-events:auto crosshair layer on top of the page
       * (see ovCursor). CSS !important + documentElement cursor are kept as a
       * fallback for the frame before the overlay mounts. */
      if (on) ensureOverlay();
      if (ovCursor) ovCursor.style.display = on ? "block" : "none";

      var id = "__liuPickCursor";
      var s = document.getElementById(id);
      if (on) {
        if (!s) {
          s = document.createElement("style");
          s.id = id;
          s.setAttribute("data-liu-pick", "1");
          s.textContent = "*,*::before,*::after{cursor:crosshair !important}";
          (document.head || document.documentElement).appendChild(s);
        }
      } else if (s && s.parentNode) {
        s.parentNode.removeChild(s);
      }
      document.documentElement.style.cursor = on ? "crosshair" : "";
    } catch (e) {}
  }

  /* Listeners install lazily on the FIRST pick activation: pages where the
   * user never picks pay zero per-event overhead. Kept installed after
   * that; the !mode early-outs make a finished pick session cost one
   * boolean check per event. */
  var listenersOn = false;
  function installListeners() {
    if (listenersOn) return;
    listenersOn = true;
    window.addEventListener("pointermove", onMove, true);
    window.addEventListener("click", onClick, true);
    window.addEventListener("keydown", onKey, true);
    window.addEventListener("scroll", onReflow, true);
    window.addEventListener("resize", onReflow, true);
  }

  window.__liuPickSetMode = function (on, token) {
    requestToken = on ? token : "";
    mode = !!on;
    if (mode) installListeners();
    if (!mode) hideOverlay();
    setPickCursor(mode);
  };
})();
