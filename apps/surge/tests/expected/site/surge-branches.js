// <surge-branches> holds a <details> menu of the page's branch, with a link to the branch list. It
// fills the menu from the repository's branches.json, named by its data-src, with a link to the
// root of each branch that has pages. Without JavaScript, or when the fetch fails, the menu shows
// only the page's branch and the link to the branch list.
class SurgeBranches extends HTMLElement {
  async connectedCallback() {
    const source = new URL(this.dataset.src, location.href);
    const list = this.querySelector("ul");
    try {
      const response = await fetch(source);
      if (!response.ok) {
        return;
      }
      const branches = await response.json();
      list.replaceChildren(
        ...branches
          .filter((branch) => branch.tree_url !== "")
          .map((branch) => {
            const link = document.createElement("a");
            link.href = new URL(branch.tree_url, source).href;
            link.textContent = branch.is_default ? `${branch.name} (default)` : branch.name;
            const item = document.createElement("li");
            item.append(link);
            return item;
          }),
      );
    } catch {
      // The menu keeps what the page holds without JavaScript.
    }
  }
}

customElements.define("surge-branches", SurgeBranches);
