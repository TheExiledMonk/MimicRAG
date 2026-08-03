package mimicrag

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"
)

type Client struct {
	BaseURL, APIKey string
	HTTP            *http.Client
}

func New(baseURL, apiKey string) *Client {
	return &Client{strings.TrimRight(baseURL, "/"), apiKey, &http.Client{Timeout: 30 * time.Second}}
}

func (c *Client) Post(path string, payload any) (map[string]any, error) {
	body, err := json.Marshal(payload)
	if err != nil { return nil, err }
	req, err := http.NewRequest("POST", c.BaseURL+path, bytes.NewReader(body))
	if err != nil { return nil, err }
	req.Header.Set("Content-Type", "application/json")
	if c.APIKey != "" { req.Header.Set("Authorization", "Bearer "+c.APIKey) }
	res, err := c.HTTP.Do(req)
	if err != nil { return nil, err }
	defer res.Body.Close()
	var value map[string]any
	if err = json.NewDecoder(res.Body).Decode(&value); err != nil { return nil, err }
	if res.StatusCode >= 400 { return nil, fmt.Errorf("mimicrag HTTP %d: %v", res.StatusCode, value["error"]) }
	return value, nil
}

func (c *Client) Retrieve(query string, topK int) (map[string]any, error) {
	return c.Post("/v1/retrieve", map[string]any{"query": query, "top_k": topK})
}

func (c *Client) Answer(query string) (map[string]any, error) {
	return c.Post("/v1/answers", map[string]any{"query": query})
}
