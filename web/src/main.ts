import "./styles.css";
import { store } from "./state";
import { Router } from "./router";
import { AppComponent } from "./components/app";

function bootstrap() {
  const rootElem = document.getElementById("app");
  if (!rootElem) {
    throw new Error("Root element #app not found");
  }

  // 1. Mount UI application first so components and their store subscribers exist
  const app = new AppComponent(rootElem, store);

  // 2. Initialize URL Router to listen to popstate/hashchange and apply initial hash
  const router = new Router(store);
  router.init();

  // 3. Reconcile initial route after mounting the components
  app.reconcile();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", bootstrap);
} else {
  bootstrap();
}
