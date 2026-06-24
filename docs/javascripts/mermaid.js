(function () {
  function renderMermaid() {
    if (typeof mermaid === "undefined") {
      return;
    }

    var scheme = document.body.getAttribute("data-md-color-scheme");
    mermaid.initialize({
      startOnLoad: false,
      theme: scheme === "slate" ? "dark" : "default"
    });

    document.querySelectorAll("pre.mermaid").forEach(function (block) {
      if (block.dataset.llamMermaidPrepared === "1") {
        return;
      }

      var graph = document.createElement("div");
      graph.className = "mermaid";
      graph.textContent = block.textContent;
      graph.dataset.llamMermaidPrepared = "1";
      block.replaceWith(graph);
    });

    mermaid.run({ querySelector: ".mermaid" });
  }

  if (typeof document$ !== "undefined") {
    document$.subscribe(renderMermaid);
  } else {
    document.addEventListener("DOMContentLoaded", renderMermaid);
  }
})();
