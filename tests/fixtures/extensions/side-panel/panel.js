document.getElementById("which").textContent = location.pathname + location.search; browser.runtime.sendMessage({ panel: location.pathname + location.search, width: innerWidth });
