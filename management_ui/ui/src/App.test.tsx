import { describe, expect, it, vi } from "vitest";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import App, { parseMongoPredicates, parseSqlQuery } from "./App";

describe("App", () => {
  it("parses supported query compatibility forms", () => {
    expect(parseMongoPredicates('{"value":{"$gt":10}}')).toEqual([{ field: "value", op: "gt", value: "10" }]);
    expect(parseSqlQuery("SELECT value FROM events WHERE value >= 10 LIMIT 5")).toMatchObject({ dataset: "events", columns: ["value"], limit: 5 });
  });

  it("requires an identity key before connecting", async () => {
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true, config: {} }) }));
    render(<App />); fireEvent.click(screen.getByText("Connect"));
    expect(await screen.findByText("Identity key path is required.")).toBeInTheDocument();
  });

  it("reviews and confirms pending memory", async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce({ ok: true, json: async () => ({ ok: true, config: {} }) })
      .mockResolvedValueOnce({ ok: true, json: async () => ({ memories: [{ memory_id: "mem-1", subject: "style", status: "pending_confirmation", namespace: "preference", sensitivity: "personal" }] }) })
      .mockResolvedValueOnce({ ok: true, json: async () => ({ memory_id: "mem-1", status: "active" }) })
      .mockResolvedValueOnce({ ok: true, json: async () => ({ memories: [] }) });
    vi.stubGlobal("fetch", fetchMock as unknown as typeof fetch); render(<App />);
    fireEvent.click(screen.getByText("Memory")); fireEvent.click(screen.getByText("Refresh"));
    expect(await screen.findByText("style")).toBeInTheDocument(); fireEvent.click(screen.getByText("Confirm"));
    await waitFor(() => expect(fetchMock).toHaveBeenCalledWith("/api/memory/action", expect.objectContaining({ method: "POST" })));
  });

  it("runs and reviews dream-state refinements", async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce({ ok: true, json: async () => ({ ok: true, config: {} }) })
      .mockResolvedValueOnce({ ok: true, json: async () => ({ cycle_id: "dream-1" }) })
      .mockResolvedValueOnce({ ok: true, json: async () => ({ refinements: [{ refinement_id: "ref-1", memory_id: "mem-1", operation: "categorize", reason: "Structured memory", confidence: .9, status: "pending_review", patch: {} }] }) });
    vi.stubGlobal("fetch", fetchMock as unknown as typeof fetch); render(<App />);
    fireEvent.click(screen.getByText("Memory")); fireEvent.click(screen.getByText("Run dream cycle"));
    expect(await screen.findByText("Structured memory")).toBeInTheDocument();
    expect(fetchMock).toHaveBeenCalledWith("/api/dream/run", expect.objectContaining({ method: "POST" }));
  });
});
