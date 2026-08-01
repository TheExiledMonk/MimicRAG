import { describe, expect, it, vi } from "vitest";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import App from "./App";

function mockFetchOnce(response: unknown, ok = true) {
  return vi.fn().mockResolvedValue({
    ok,
    json: async () => response,
  });
}

describe("App", () => {
  it("shows connection errors", async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({ ok: true, version: "0.1.0", config: {} }),
      })
      .mockResolvedValueOnce({
        ok: false,
        json: async () => ({ detail: "connect failed" }),
      });

    vi.stubGlobal("fetch", fetchMock as unknown as typeof fetch);
    render(<App />);

    fireEvent.click(screen.getByText("Connect"));

    await waitFor(() => {
      expect(screen.getByText("connect failed")).toBeInTheDocument();
    });
  });

  it("sends predicates and shows next page", async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({ ok: true, version: "0.1.0", config: {} }),
      })
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({ ok: true }),
      })
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({ databases: ["default"] }),
      })
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({ datasets: ["events"] }),
      })
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({
          fields: [
            { name: "value", type: "int64", nullable: false, encoding: "raw" },
          ],
        }),
      })
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ({
          columns: ["value"],
          rows: [[1]],
          cursor: "next",
          has_more: true,
        }),
      });

    vi.stubGlobal("fetch", fetchMock as unknown as typeof fetch);
    render(<App />);

    fireEvent.click(screen.getByText("Connect"));
    await waitFor(() => {
      expect(fetchMock).toHaveBeenCalledWith("/api/databases");
    });

    fireEvent.click(screen.getByText("Refresh"));
    await waitFor(() => {
      expect(fetchMock).toHaveBeenCalledWith("/api/datasets?database=default");
    });

    fireEvent.click(screen.getByText("events"));

    fireEvent.click(screen.getByText("Data Preview"));

    const addButton = await screen.findByText("Add");
    fireEvent.click(addButton);

    const valueInput = screen.getByPlaceholderText("value");
    fireEvent.change(valueInput, { target: { value: "10" } });

    fireEvent.click(screen.getByText("Run"));

    await waitFor(() => {
      expect(screen.getByText("Next Page")).toBeInTheDocument();
    });

    const scanCall = fetchMock.mock.calls.find((call) => call[0] === "/api/scan");
    expect(scanCall).toBeTruthy();
    const scanPayload = JSON.parse(scanCall?.[1]?.body as string);
    expect(scanPayload.predicates.length).toBe(1);
  });

  it("applies mongo query input", async () => {
    vi.stubGlobal("fetch", mockFetchOnce({ ok: true, version: "0.1.0", config: {} }) as unknown as typeof fetch);
    render(<App />);

    fireEvent.click(screen.getByText("Data Preview"));
    fireEvent.change(screen.getByDisplayValue("Mimic Native"), {
      target: { value: "mongo" },
    });

    const textarea = screen.getByPlaceholderText(
      '{ "value": { "$gt": 10 }, "$and": [{"ts": {"$lt": 100}}] }'
    );
    fireEvent.change(textarea, {
      target: { value: '{ "value": { "$gt": 10 } }' },
    });

    fireEvent.click(screen.getByText("Apply Query"));

    await waitFor(() => {
      expect(screen.getByText("Predicates are parsed from the query input.")).toBeInTheDocument();
    });
  });
});
