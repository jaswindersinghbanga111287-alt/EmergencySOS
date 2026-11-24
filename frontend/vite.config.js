import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The C++ backend runs on port 8080. During development, Vite forwards
// every /api request to it, so the browser only talks to one address.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    open: true,
    proxy: {
      "/api": process.env.API_URL || "http://localhost:8080",
    },
  },
});
