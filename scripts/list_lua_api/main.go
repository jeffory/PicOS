package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

type Function struct {
	Name       string
	Module     string
	Documented bool
}

type Constant struct {
	Name       string
	Module     string
	Documented bool
}

type Module struct {
	Name      string
	DocFile   string
	Functions []Function
	Constants []Constant
}

var (
	outputPath       string
	undocumentedOnly bool
	showSummary      bool
)

var docFileMapping = map[string]string{
	"display":  "API-Display-and-Graphics.md",
	"input":    "API-Input.md",
	"sys":      "API-System-and-Config.md",
	"config":   "API-System-and-Config.md",
	"fs":       "API-Filesystem.md",
	"wifi":     "API-Network-and-WiFi.md",
	"network":  "API-Network-and-WiFi.md",
	"ui":       "API-UI.md",
	"audio":    "API-Audio-and-Sound.md",
	"sound":    "API-Audio-and-Sound.md",
	"perf":     "API-Performance.md",
	"graphics": "API-Display-and-Graphics.md",
	"video":     "API-Video.md",
	"repl":      "API-Repl.md",
	"terminal":  "API-Terminal.md",
	"crypto":    "API-Crypto.md",
	"modplayer": "API-Modplayer.md",
	"sysconfig": "API-Sysconfig.md",
}

func main() {
	flag.StringVar(&outputPath, "output", "", "Output file path (default: stdout)")
	flag.BoolVar(&undocumentedOnly, "undocumented-only", false, "Show only undocumented items")
	flag.BoolVar(&showSummary, "summary", true, "Show summary counts")
	flag.Parse()

	modules := parseLuaBridges()
	docContents := readDocFiles()

	for i := range modules {
		checkDocumentation(&modules[i], docContents)
	}

	output(modules)
}

func parseLuaBridges() []Module {
	modules := make(map[string]*Module)

	entries, err := os.ReadDir("src/os")
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error reading src/os: %v\n", err)
		os.Exit(1)
	}

	for _, entry := range entries {
		if !entry.IsDir() && strings.HasPrefix(entry.Name(), "lua_bridge") && strings.HasSuffix(entry.Name(), ".c") {
			parseLuaBridgeFile(filepath.Join("src/os", entry.Name()), modules)
		}
	}

	result := make([]Module, 0, len(modules))
	for _, m := range modules {
		result = append(result, *m)
	}
	sort.Slice(result, func(i, j int) bool {
		return result[i].Name < result[j].Name
	})

	return result
}

func parseLuaBridgeFile(path string, modules map[string]*Module) {
	content, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error reading %s: %v\n", path, err)
		return
	}

	contentStr := string(content)

	moduleReg := regexp.MustCompile(`register_subtable\s*\(\s*L\s*,\s*"(\w+)"`)
	matches := moduleReg.FindAllStringSubmatch(contentStr, -1)

	moduleNames := []string{}
	for _, match := range matches {
		if len(match) > 1 {
			moduleName := match[1]
			moduleNames = append(moduleNames, moduleName)
			if _, ok := modules[moduleName]; !ok {
				modules[moduleName] = &Module{
					Name:      moduleName,
					DocFile:   docFileMapping[moduleName],
					Functions: []Function{},
					Constants: []Constant{},
				}
			}
		}
	}

	if len(moduleNames) == 0 {
		return
	}

	fileConstReg := regexp.MustCompile(`lua_pushinteger\s*\(\s*L\s*,\s*(\w+)\s*\)\s*;\s*\n\s*lua_setfield\s*\(\s*L\s*,\s*-\s*2\s*,\s*"(\w+)"`)
	constMatches := fileConstReg.FindAllStringSubmatch(contentStr, -1)

	constBlacklist := map[string]bool{
		"nup": true, "name": true, "version": true, "description": true,
		"author": true, "requirements": true, "L": true, "lua": true,
	}

	for _, match := range constMatches {
		if len(match) > 2 {
			val := match[1]
			constName := match[2]
			if constBlacklist[constName] {
				continue
			}
			for _, modName := range moduleNames {
				if m, ok := modules[modName]; ok {
					m.Constants = append(m.Constants, Constant{Name: constName, Module: modName})
				}
			}
			_ = val
		}
	}

	funcReg := regexp.MustCompile(`\{\s*"(\w+)"\s*,\s*l_(\w+)_`)
	funcMatches := funcReg.FindAllStringSubmatch(contentStr, -1)

	for _, match := range funcMatches {
		if len(match) > 2 {
			funcName := match[1]
			possibleModule := match[2]
			for _, modName := range moduleNames {
				if m, ok := modules[modName]; ok {
					if possibleModule == modName {
						m.Functions = append(m.Functions, Function{Name: funcName, Module: modName})
					}
				}
			}
		}
	}

	funcReg2 := regexp.MustCompile(`\{\s*"(\w+)"\s*,\s*l_\w+\}`)
	funcMatches2 := funcReg2.FindAllStringSubmatch(contentStr, -1)

	for _, match := range funcMatches2 {
		if len(match) > 1 {
			funcName := match[1]
			exists := false
			for _, modName := range moduleNames {
				if m, ok := modules[modName]; ok {
					for _, f := range m.Functions {
						if f.Name == funcName {
							exists = true
							break
						}
					}
				}
			}
			if !exists {
				for _, modName := range moduleNames {
					if m, ok := modules[modName]; ok {
						m.Functions = append(m.Functions, Function{Name: funcName, Module: modName})
					}
				}
			}
		}
	}

	for _, modName := range moduleNames {
		if m, ok := modules[modName]; ok {
			seenFuncs := make(map[string]bool)
			uniqueFuncs := []Function{}
			for _, f := range m.Functions {
				if !seenFuncs[f.Name] {
					seenFuncs[f.Name] = true
					uniqueFuncs = append(uniqueFuncs, f)
				}
			}
			m.Functions = uniqueFuncs

			seenConsts := make(map[string]bool)
			uniqueConsts := []Constant{}
			for _, c := range m.Constants {
				if !seenConsts[c.Name] {
					seenConsts[c.Name] = true
					uniqueConsts = append(uniqueConsts, c)
				}
			}
			m.Constants = uniqueConsts
		}
	}
}

func readDocFiles() map[string]string {
	contents := make(map[string]string)
	entries, err := os.ReadDir("docs")
	if err != nil {
		return contents
	}

	for _, entry := range entries {
		if !entry.IsDir() && strings.HasPrefix(entry.Name(), "API-") && strings.HasSuffix(entry.Name(), ".md") {
			path := filepath.Join("docs", entry.Name())
			content, err := os.ReadFile(path)
			if err == nil {
				contents[entry.Name()] = string(content)
			}
		}
	}
	return contents
}

func checkDocumentation(module *Module, docContents map[string]string) {
	if module.DocFile == "" {
		for i := range module.Functions {
			module.Functions[i].Documented = false
		}
		for i := range module.Constants {
			module.Constants[i].Documented = false
		}
		return
	}

	docContent, exists := docContents[module.DocFile]
	if !exists {
		for i := range module.Functions {
			module.Functions[i].Documented = false
		}
		for i := range module.Constants {
			module.Constants[i].Documented = false
		}
		return
	}

	for i := range module.Functions {
		fname := module.Functions[i].Name
		searchPatterns := []string{
			fmt.Sprintf("picocalc.%s.%s(", module.Name, fname),
			fmt.Sprintf("`picocalc.%s.%s(", module.Name, fname),
			fmt.Sprintf(":%s(", fname),  // method syntax e.g. player:load(
		}
		found := false
		for _, pattern := range searchPatterns {
			if strings.Contains(docContent, pattern) {
				found = true
				break
			}
		}
		module.Functions[i].Documented = found
	}

	for i := range module.Constants {
		searchPatterns := []string{
			fmt.Sprintf("picocalc.%s.%s", module.Name, module.Constants[i].Name),
			fmt.Sprintf("`picocalc.%s.%s`", module.Name, module.Constants[i].Name),
			fmt.Sprintf("**%s**", module.Constants[i].Name),
		}
		found := false
		for _, pattern := range searchPatterns {
			if strings.Contains(docContent, pattern) {
				found = true
				break
			}
		}
		module.Constants[i].Documented = found
	}
}

func output(modules []Module) {
	var out *os.File
	var err error

	if outputPath != "" {
		out, err = os.Create(outputPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error creating output file: %v\n", err)
			os.Exit(1)
		}
		defer out.Close()
	} else {
		out = os.Stdout
	}

	fmt.Fprintln(out, "# PicOS Lua SDK Manifest")
	fmt.Fprintln(out, "")
	fmt.Fprintln(out, "Auto-generated by `scripts/list_lua_api/main.go`")
	fmt.Fprintln(out, "")

	totalFuncs := 0
	totalDocFuncs := 0
	totalConsts := 0
	totalDocConsts := 0

	for _, module := range modules {
		funcs := module.Functions
		consts := module.Constants

		if undocumentedOnly {
			filteredFuncs := []Function{}
			for _, f := range funcs {
				if !f.Documented {
					filteredFuncs = append(filteredFuncs, f)
				}
			}
			funcs = filteredFuncs

			filteredConsts := []Constant{}
			for _, c := range consts {
				if !c.Documented {
					filteredConsts = append(filteredConsts, c)
				}
			}
			consts = filteredConsts
		}

		if len(funcs) == 0 && len(consts) == 0 {
			continue
		}

		fmt.Fprintf(out, "## picocalc.%s\n", module.Name)
		fmt.Fprintln(out, "")

		if len(funcs) > 0 {
			fmt.Fprintln(out, "### Functions")
			fmt.Fprintln(out, "")
			fmt.Fprintln(out, "| Function | Status |")
			fmt.Fprintln(out, "|----------|--------|")
			sort.Slice(funcs, func(i, j int) bool {
				return funcs[i].Name < funcs[j].Name
			})
			for _, f := range funcs {
				status := "✅ documented"
				if !f.Documented {
					status = "❌ undocumented"
				}
				fmt.Fprintf(out, "| %s | %s |\n", f.Name, status)
			}
			fmt.Fprintln(out, "")
		}

		if len(consts) > 0 {
			fmt.Fprintln(out, "### Constants")
			fmt.Fprintln(out, "")
			fmt.Fprintln(out, "| Constant | Status |")
			fmt.Fprintln(out, "|----------|--------|")
			sort.Slice(consts, func(i, j int) bool {
				return consts[i].Name < consts[j].Name
			})
			for _, c := range consts {
				status := "✅ documented"
				if !c.Documented {
					status = "❌ undocumented"
				}
				fmt.Fprintf(out, "| %s | %s |\n", c.Name, status)
			}
			fmt.Fprintln(out, "")
		}

		fmt.Fprintln(out, "---")
		fmt.Fprintln(out, "")

		if !undocumentedOnly {
			docFuncs := 0
			for _, f := range module.Functions {
				if f.Documented {
					docFuncs++
				}
			}
			docConsts := 0
			for _, c := range module.Constants {
				if c.Documented {
					docConsts++
				}
			}
			totalFuncs += len(module.Functions)
			totalDocFuncs += docFuncs
			totalConsts += len(module.Constants)
			totalDocConsts += docConsts
		}
	}

	if showSummary && !undocumentedOnly {
		fmt.Fprintln(out, "## Summary")
		fmt.Fprintln(out, "")
		fmt.Fprintln(out, "| Module | Functions | Documented | Constants | Documented |")
		fmt.Fprintln(out, "|--------|-----------|------------|-----------|------------|")
		for _, module := range modules {
			docFuncs := 0
			for _, f := range module.Functions {
				if f.Documented {
					docFuncs++
				}
			}
			docConsts := 0
			for _, c := range module.Constants {
				if c.Documented {
					docConsts++
				}
			}

			funcPct := 0
			if len(module.Functions) > 0 {
				funcPct = (docFuncs * 100) / len(module.Functions)
			}
			constPct := 0
			if len(module.Constants) > 0 {
				constPct = (docConsts * 100) / len(module.Constants)
			}

			constStr := fmt.Sprintf("%d", len(module.Constants))
			if len(module.Constants) == 0 {
				constStr = "-"
				constPct = 100
			}

			fmt.Fprintf(out, "| %s | %d | %d (%d%%) | %s | %d%% |\n",
				module.Name, len(module.Functions), docFuncs, funcPct, constStr, constPct)
		}

		totalFuncPct := 0
		if totalFuncs > 0 {
			totalFuncPct = (totalDocFuncs * 100) / totalFuncs
		}
		totalConstPct := 0
		if totalConsts > 0 {
			totalConstPct = (totalDocConsts * 100) / totalConsts
		}

		fmt.Fprintf(out, "| **Total** | **%d** | **%d (%d%%)** | **%d** | **%d%%** |\n",
			totalFuncs, totalDocFuncs, totalFuncPct, totalConsts, totalConstPct)
		fmt.Fprintln(out, "")
	}
}
