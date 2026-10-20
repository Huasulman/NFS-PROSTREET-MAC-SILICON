// Prepara el proyecto de Ghidra de NFS ProStreet a partir de analysis/ghidra_labels.tsv:
// divide la memoria en las secciones del XEX, crea las funciones que conoce XenonRecomp,
// pone nombre a los imports y a las funciones de la CRT, y marca los casos para revisar.
//
// Se ejecuta como -preScript de analyzeHeadless (ver tools/ghidra/create_project.sh),
// con la ruta del TSV como argumento.
//@category ProStreet

import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class ProStreetSetup extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            printerr("Uso: ProStreetSetup.java <ghidra_labels.tsv>");
            return;
        }

        List<String[]> rows = new ArrayList<>();
        try (BufferedReader reader = new BufferedReader(new FileReader(args[0]))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (!line.isEmpty()) {
                    rows.add(line.split("\t"));
                }
            }
        }

        Memory memory = currentProgram.getMemory();
        int sections = 0, functions = 0, labels = 0, bookmarks = 0;

        for (String[] row : rows) {
            if (!row[0].equals("section")) {
                continue;
            }
            Address start = toAddr(row[1]);
            Address end = toAddr(row[2]);
            splitAt(memory, start);
            splitAt(memory, end);
            MemoryBlock block = memory.getBlock(start);
            block.setName(row[3]);
            block.setRead(row[4].contains("r"));
            block.setWrite(row[4].contains("w"));
            block.setExecute(row[4].contains("x"));
            sections++;
        }

        // Disassemble every known entry point first, then create the functions.
        AddressSet entries = new AddressSet();
        for (String[] row : rows) {
            if (row[0].equals("function")) {
                entries.add(toAddr(row[1]));
            }
        }
        monitor.setMessage("Desensamblando " + entries.getNumAddresses() + " funciones");
        new DisassembleCommand(entries, null, true).applyTo(currentProgram, monitor);

        for (String[] row : rows) {
            monitor.checkCancelled();
            Address address = toAddr(row[1]);
            switch (row[0]) {
                case "function": {
                    Function function = getFunctionAt(address);
                    if (function == null) {
                        function = createFunction(address, row[2]);
                    }
                    if (function != null) {
                        function.setName(row[2], SourceType.IMPORTED);
                        functions++;
                    } else {
                        createLabel(address, row[2], true);
                    }
                    break;
                }
                case "label":
                    createLabel(address, row[2], true);
                    labels++;
                    break;
                case "bookmark":
                    createBookmark(address, row[2], row[3]);
                    setPreComment(address, row[2] + ": " + row[3]);
                    bookmarks++;
                    break;
                default:
                    break;
            }
        }

        println(String.format("Secciones: %d, funciones: %d, etiquetas: %d, marcadores: %d",
            sections, functions, labels, bookmarks));
    }

    private void splitAt(Memory memory, Address address) throws Exception {
        MemoryBlock block = memory.getBlock(address);
        if (block != null && !block.getStart().equals(address)) {
            memory.split(block, address);
        }
    }
}
