use serde_json::{json, Value};

pub struct Client { base_url: String, api_key: String, http: ureq::Agent }
impl Client {
    pub fn new(base_url: &str, api_key: &str) -> Self { Self { base_url: base_url.trim_end_matches('/').into(), api_key: api_key.into(), http: ureq::agent() } }
    fn post(&self, path: &str, value: Value) -> Result<Value, ureq::Error> {
        let mut request = self.http.post(&(self.base_url.clone() + path)).header("content-type", "application/json");
        if !self.api_key.is_empty() { request = request.header("authorization", &format!("Bearer {}", self.api_key)); }
        request.send_json(value)?.body_mut().read_json()
    }
    pub fn retrieve(&self, query: &str, top_k: usize) -> Result<Value, ureq::Error> { self.post("/v1/retrieve", json!({"query": query, "top_k": top_k})) }
    pub fn answer(&self, query: &str) -> Result<Value, ureq::Error> { self.post("/v1/answers", json!({"query": query})) }
}
