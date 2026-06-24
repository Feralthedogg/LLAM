const setupLlamInstallCopy = () => {
  document.querySelectorAll(".llam-copy-button").forEach((button) => {
    if (button.dataset.copyBound === "true") {
      return;
    }

    button.dataset.copyBound = "true";
    button.addEventListener("click", async () => {
      const command = button.dataset.copyCommand;

      if (!command) {
        return;
      }

      try {
        if (navigator.clipboard && window.isSecureContext) {
          await navigator.clipboard.writeText(command);
        } else {
          const textarea = document.createElement("textarea");
          textarea.value = command;
          textarea.setAttribute("readonly", "");
          textarea.style.position = "fixed";
          textarea.style.opacity = "0";
          document.body.appendChild(textarea);
          textarea.select();
          const copied = document.execCommand("copy");
          document.body.removeChild(textarea);

          if (!copied) {
            throw new Error("Copy command was rejected.");
          }
        }

        button.textContent = "Copied";
        button.dataset.copyState = "copied";
      } catch (error) {
        button.textContent = "Failed";
        button.dataset.copyState = "failed";
      }

      window.setTimeout(() => {
        button.textContent = "Copy";
        delete button.dataset.copyState;
      }, 1400);
    });
  });
};

if (typeof document$ !== "undefined") {
  document$.subscribe(setupLlamInstallCopy);
} else {
  document.addEventListener("DOMContentLoaded", setupLlamInstallCopy);
}
