from pathlib import Path
import sys
root=Path(sys.argv[1]).resolve()
(root/'ins/src/analysis.hpp').write_text('''#pragma once
#include <nlohmann/json.hpp>
namespace ins::audit { extern double history; void emit(nlohmann::json value); }
''')
p=root/'ins/src/alignment.cpp';s=p.read_text().replace('#include "ins/alignment.hpp"','#include "ins/alignment.hpp"\n#include "analysis.hpp"')
needle='''    if (fixes.size() < 4)
        return {};'''
s=s.replace(needle,'''    nlohmann::json trace{{"event", "align"}, {"time", seconds(imu.back().stamp.measurement)}, {"fixes", fixes.size()}};
    auto reject = [&](const char *reason) -> std::optional<NavigationState> {
        trace["reason"] = reason; audit::emit(trace); return {};
    };
    if (fixes.size() < 4)
        return reject("too_few_fixes");''')
s=s.replace('''    const double duration = seconds(end - start);''','''    const double duration = seconds(end - start);
    trace["duration"] = duration;''')
s=s.replace('''    const Quat attitude =''','''    trace["svd"] = {svd.singularValues()[0], svd.singularValues()[1], svd.singularValues()[2]};
    const Quat attitude =''',1)
s=s.replace('''    if (information_factor.info() != Eigen::Success)
        return {};''','''    if (information_factor.info() != Eigen::Success)
        return reject("singular_information");''')
s=s.replace('''    if (lower.info() != Eigen::Success ||''','''    trace["gnss_sigma_deg"] = std::sqrt(std::max(0., lower.eigenvalues().maxCoeff())) / degrees;
    if (lower.info() != Eigen::Success ||''')
s=s.replace('''        lower.eigenvalues().maxCoeff() > std::pow(10 * degrees, 2))
        return {};''','''        lower.eigenvalues().maxCoeff() > std::pow(10 * degrees, 2))
        return reject("gnss_uncertainty");''')
s=s.replace('''    if (uncertainty.info() != Eigen::Success ||''','''    trace["total_sigma_deg"] = std::sqrt(std::max(0., uncertainty.eigenvalues().maxCoeff())) / degrees;
    if (uncertainty.info() != Eigen::Success ||''')
s=s.replace('''        uncertainty.eigenvalues().maxCoeff() > std::pow(10 * degrees, 2))
        return {};''','''        uncertainty.eigenvalues().maxCoeff() > std::pow(10 * degrees, 2))
        return reject("total_uncertainty");''')
s=s.replace('''    if (residual.dot(noise_factor.solve(residual)) > dof + 2 * std::sqrt(dof * tail) + 2 * tail)
        return {};''','''    trace["residual_chi2"] = residual.dot(noise_factor.solve(residual));
    trace["residual_limit"] = dof + 2 * std::sqrt(dof * tail) + 2 * tail;
    if (trace["residual_chi2"].get<double>() > trace["residual_limit"].get<double>())
        return reject("residual_inconsistent");''')
i=s.rfind('    return out;');s=s[:i]+'    trace[\"reason\"] = \"accepted\"; audit::emit(trace);\n'+s[i:]
p.write_text(s)
# Coarse retention is now an ordinary production Config parameter, set by audit.cpp.
p=root/'ins/src/ukf.cpp';s=p.read_text().replace('#include "ins/ukf.hpp"','#include "ins/ukf.hpp"\n#include "analysis.hpp"');s=s.replace('''    // Chi-squared(6), 99.99% acceptance''','''    const auto residual = (observed - mean).eval();
    audit::emit({{"event", "innovation"}, {"time", seconds(fix.stamp.measurement)},
                 {"nis", innovation_squared_},
                 {"residual", {residual[0], residual[1], residual[2], residual[3], residual[4], residual[5]}},
                 {"variance", {innovation(0,0), innovation(1,1), innovation(2,2), innovation(3,3), innovation(4,4), innovation(5,5)}},
                 {"accepted", std::isfinite(innovation_squared_) && innovation_squared_ <= 27.856341236}});
    // Chi-squared(6), 99.99% acceptance''');p.write_text(s)
assert 'return {};' not in (root/'ins/src/alignment.cpp').read_text().replace('audit::emit(trace); return {};','')
