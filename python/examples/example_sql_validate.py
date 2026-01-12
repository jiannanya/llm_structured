from __future__ import annotations

import json

from llm_structured import ValidationError, parse_and_validate_sql


def main() -> None:
    llm_output = """
    当然可以，下面是 SQL：

    ```sql
    SELECT id, email
    FROM users
    WHERE is_active = 1
    ORDER BY id DESC
    LIMIT 100;
    ```
    """

    schema = {
        # 安全默认：只允许 SELECT
        "allowedStatements": ["select"],
        "requireWhere": True,
        "requireLimit": True,
        "maxLimit": 200,
        "forbidUnion": True,
        "forbidSelectStar": True,
        "forbidSelectWithoutLimit": True,
        # 典型多租户/权限约束：WHERE 必须包含某个列
        "requireWhereColumns": ["is_active"],
        # 更严格：WHERE 必须匹配某个模式（比如 tenant_id / org_id 等）
        # 这里演示 is_active= 的模式约束
        "requireWherePatterns": [r"\\bis_active\\s*=\\s*"],
        # 允许的表白名单（可选）
        "allowedTables": ["users"],
    }

    try:
        parsed = parse_and_validate_sql(llm_output, schema)
    except ValidationError as e:
        print("SQL validation failed:", e)
        raise

    print("SQL validated. Parsed summary:")
    print(
        json.dumps(
            {
                "statementType": parsed["statementType"],
                "hasWhere": parsed["hasWhere"],
                "hasLimit": parsed["hasLimit"],
                "limit": parsed["limit"],
                "tables": parsed["tables"],
            },
            indent=2,
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
