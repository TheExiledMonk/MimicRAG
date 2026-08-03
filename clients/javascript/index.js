export class MimicRagClient {
  constructor(baseUrl = "http://127.0.0.1:8080", apiKey = "") { this.baseUrl = baseUrl.replace(/\/$/, ""); this.apiKey = apiKey; }
  async request(method, path, body) {
    const response = await fetch(this.baseUrl + path, {method, headers: {"content-type": "application/json", ...(this.apiKey ? {authorization: `Bearer ${this.apiKey}`} : {})}, body: body === undefined ? undefined : JSON.stringify(body)});
    const value = await response.json(); if (!response.ok) throw new Error(value.error || `HTTP ${response.status}`); return value;
  }
  retrieve(query, options = {}) { return this.request("POST", "/v1/retrieve", {query, ...options}); }
  answer(query, options = {}) { return this.request("POST", "/v1/answers", {query, ...options}); }
  ingest(text, source_uri, options = {}) { return this.request("POST", "/v1/documents", {text, source_uri, ...options}); }
  trace(id) { return this.request("GET", `/v1/traces/${encodeURIComponent(id)}`); }
}
