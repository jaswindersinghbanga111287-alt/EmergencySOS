// All requests go to the C++ backend through /api (see vite.config.js).
import { useCallback, useEffect, useState } from "react";

export async function api(path, options = {}) {
  let response;
  try {
    response = await fetch(`/api${path}`, {
      ...options,
      headers: { "Content-Type": "application/json", ...(options.headers || {}) },
    });
  } catch {
    throw new Error("Cannot reach the C++ backend. Make sure it is running on port 8080.");
  }
  const text = await response.text();
  let data = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch {
    data = { message: text };
  }
  if (!response.ok) {
    if (response.status === 502 || response.status === 504 || (response.status === 500 && !data?.message)) {
      throw new Error("Cannot reach the C++ backend. Make sure it is running on port 8080.");
    }
    throw new Error(data?.message || `Request failed (${response.status})`);
  }
  return data;
}

export const post = (path, body) => api(path, { method: "POST", body: JSON.stringify(body ?? {}) });

// Loads data from the backend and reloads whenever `refreshKey` changes.
export function useApi(path, refreshKey = 0) {
  const [state, setState] = useState({ data: null, error: "", loading: true });
  const load = useCallback(async () => {
    setState((s) => ({ ...s, loading: true }));
    try {
      const data = await api(path);
      setState({ data, error: "", loading: false });
    } catch (e) {
      setState({ data: null, error: e.message, loading: false });
    }
  }, [path]);
  useEffect(() => {
    load();
  }, [load, refreshKey]);
  return { ...state, reload: load };
}

// Turns form fields into a plain object.
export function formToObject(form) {
  return Object.fromEntries(new FormData(form).entries());
}
