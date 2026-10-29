// <surge-view> holds the rendered view of a Markdown or Org file and its source. It shows the
// rendered view, or the source when the fragment names a line, such as #L12, and two buttons
// switch between them. It remembers nothing across pages. Without JavaScript both views show,
// rendered first.
class SurgeView extends HTMLElement {
  connectedCallback() {
    this.rendered = this.querySelector(".rendered");
    this.source = this.querySelector(".source");
    const bar = document.createElement("p");
    for (const [label, isSource] of [["Rendered", false], ["Source", true]]) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = label;
      button.addEventListener("click", () => this.show(isSource));
      bar.append(button, " ");
    }
    this.prepend(bar);
    this.onHashChange = () => this.show(/^#L\d+$/.test(location.hash));
    window.addEventListener("hashchange", this.onHashChange);
    this.onHashChange();
  }

  disconnectedCallback() {
    window.removeEventListener("hashchange", this.onHashChange);
  }

  show(isSource) {
    this.rendered.hidden = isSource;
    this.source.hidden = !isSource;
  }
}

customElements.define("surge-view", SurgeView);
